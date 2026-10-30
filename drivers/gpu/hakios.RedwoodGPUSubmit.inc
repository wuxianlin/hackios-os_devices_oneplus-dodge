/* Submission validation precedes cache maintenance and backend publication. */
IOReturn
RedwoodGPUUserClient::submit(struct msm_gem_submit * request)
{
	struct msm_gem_submit_bo * bos = NULL;
	struct msm_gem_submit_cmd * cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0;
	uint64_t entered = mach_absolute_time(), began, nanos, again, roomed;
	IOReturn ret = kIOReturnBadArgument;
	const bool a830 = redwood_platform_is_gpu8();

	if (a830 && ((request->flags & ~MSM_SUBMIT_NO_IMPLICIT) != MSM_PIPE_3D0 ||
	    request->in_syncobjs || request->out_syncobjs || request->nr_in_syncobjs ||
	    request->nr_out_syncobjs || request->syncobj_stride)) {
		return kIOReturnBadArgument;
	}
	if (request->nr_cmds == 0 || request->nr_cmds > (a830 ? GPU_A830_SUBMIT_CMDS : GPU_SUBMIT_CMDS) || request->nr_bos == 0 || request->nr_bos > GPU_SUBMIT_BOS) {
		IOLog("RedwoodGPU: a submission of %u command buffers in %u buffers: not taken\n", request->nr_cmds, request->nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, request->nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, request->nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t) request->bos, bos, request->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t) request->cmds, cmds, request->nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	roomed = mach_absolute_time();
	/*
	 * The A830's queue holds adreno8_context_depth() submissions ahead of the GPU. With that many
	 * outstanding, the backend would wait for the oldest with this lock held, and every other
	 * buffer request and the display's driver with it; gpuWait() lets go of the lock meanwhile.
	 */
	for (uint32_t depth = a830 ? adreno8_context_depth() : 0, last; depth != 0 && !gGPUReported && redwood_gpu_ready() &&
	    (last = redwood_gpu_last()) >= depth && !redwood_gpu_done(last - depth + 1);) {
		if (!gpuWait(last - depth + 1, GPU_PATIENCE_MS)) {
			break;                                  /* (the backend's own wait says what is wrong) */
		}
	}
	gGPUStat.rooming += mach_absolute_time() - roomed;
	if (gGPUReported || !redwood_gpu_ready()) {
		IOLockUnlock(gGPULock);
		ret = kIOReturnNotReady;
		goto out;
	}
	for (uint32_t i = 0; i < request->nr_cmds; i++) {
		struct gpu_buffer * b = cmds[i].submit_idx < request->nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		uint64_t address = b ? b->address + cmds[i].submit_offset : 0;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                               /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t) cmds[i].submit_offset + cmds[i].size > b->size ||
		    (a830 && (cmds[i].size == 0 || cmds[i].size / 4 > 0xfffff || b->address >= (1ull << 32) ||
		    address >= (1ull << 32) || cmds[i].size > (1ull << 32) - address))) {
			IOLog("RedwoodGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = address;
		words[count++] = cmds[i].size / 4;
	}
	if (count == 0) {
		IOLockUnlock(gGPULock);
		goto out;
	}
	/*
	 * The GPU reads memory and writes memory. What this processor's cache holds of an IOSurface
	 * (somebody has written it, or has read it, as the display's driver does, and the GPU is to
	 * draw into it again) goes out of the cache now: when the surface is said to have changed
	 * (its seed: who writes one is to lock it, and so does a program that has had the GPU draw
	 * into one: userland/src/eagl.c), and in any case once in a few milliseconds, which is once
	 * a frame and not once a submission: not everybody who draws into a surface says so (an
	 * application drawing a picture that is on the screen meanwhile). A surface this program
	 * has mapped, every time. The buffers made here need none of that, unless they are
	 * ordinary memory (gpu_cached=1).
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b == NULL) {
			continue;
		}
		if (b->surface != NULL) {
			uint32_t seed = b->surface->shared()->seed;

			if (seed == b->seed && !b->seen && began - b->pushed < again) {
				continue;
			}
			b->seed = seed;
			b->pushed = began;
		} else if (!b->seen || !gGPUCached) {
			continue;
		}
		redwood_gpu_to_memory(b->kernel, b->size);
		gGPUStat.flushed += b->size;
	}
	redwood_gpu_to_memory(NULL, 0);                 /* (and what was written last is in memory: a barrier) */
	gGPUStat.flushing += mach_absolute_time() - began;
	gGPUStat.buffers += request->nr_bos;
	request->fence = redwood_gpu_submit(addresses, words, count);
	/*
	 * The ring is full and the core is not reading it: it has stopped. With gpu_recover=1 it is
	 * reset and given this submission again -- after a reset the ring is empty, so there is
	 * room for it. (A wait of a second or more notices a stopped core first, as a rule; this is
	 * for a program that submits without waiting.)
	 */
	if (request->fence == 0 && !gGPUReported) {
		redwood_gpu_report("the ring is full");
		gGPUReported = !redwood_gpu_recover();
		if (!gGPUReported) {
			request->fence = redwood_gpu_submit(addresses, words, count);
		}
	}
	if (request->fence == 0) {
		/* The backend may have published commands before the failure. Keep every mapped
		 * descriptor, including buffers still carrying their previous fence. */
		gGPUReported = true;
	}
	if (gpu_log && request->fence != 0) {
		/* "tl: Q FENCE WHO at US: N buffers, B of 4 MB or more, W words of commands": what was given
		 * (tl: q: what it took). WHO is the process that gave it, because several share the GPU and
		 * the sum alone does not say which of them is the one that submits a hundred times a frame. */
		uint32_t big = 0, all_words = 0;
		uint64_t ns;
		char who[24] = "?";

		for (uint32_t i = 0; i < request->nr_bos; i++) {
			struct gpu_buffer * b = buffer(bos[i].handle);

			big += b != NULL && b->size >= (4u << 20);
		}
		for (uint32_t i = 0; i < count; i++) {
			all_words += words[i];
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		proc_selfname(who, sizeof(who));
		redwood_tl("tl: Q %u %s at %llu: %u buffers, %u big, %u words\n", request->fence, who, ns / 1000,
		    request->nr_bos, big, all_words);
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	/*
	 * The surfaces named are being drawn into, or read, until this submission is done: the
	 * display's driver waits for that before it puts one on the panel (MiniIOSurfaceDrawn()).
	 * (freedreno names every buffer for reading and writing alike, so which it is cannot be
	 * told; the last submission of a frame names the surface the frame is drawn into.)
	 */
	for (uint32_t i = 0; request->fence != 0 && i < request->nr_bos; i++) {
		struct gpu_buffer * b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
			if (b->surface != NULL) {
				b->surface->drawn = request->fence;
			}
		}
	}
	reap();
	if (fSubmits++ < 4) {
		IOLog("RedwoodGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	/* every five seconds in which something was submitted: how much, and what it cost here */
	gGPUStat.submissions++;
	gGPUStat.submitting += mach_absolute_time() - entered;
	gGPUStat.commands += count;
	if (gGPUStat.since == 0) {
		gGPUStat.since = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - gGPUStat.since, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t flushing, waiting, taking, worked, submitting, rooming;
		uint32_t timed, by[7];

		absolutetime_to_nanoseconds(gGPUStat.submitting, &submitting);
		absolutetime_to_nanoseconds(gGPUStat.rooming, &rooming);
		absolutetime_to_nanoseconds(gGPUStat.flushing, &flushing);
		absolutetime_to_nanoseconds(gGPUStat.waiting, &waiting);
		absolutetime_to_nanoseconds(gGPUStat.taking, &taking);
		redwood_gpu_worked(&worked, &timed, by);
		uint64_t making, allocating, locking;

		absolutetime_to_nanoseconds(gGPUStat.making, &making);
		absolutetime_to_nanoseconds(gGPUStat.allocating, &allocating);
		absolutetime_to_nanoseconds(gGPUStat.locking, &locking);
		IOLog("RedwoodGPU: buffers: %u made in %llu us each (%llu getting the memory, %llu waiting for the GPU lock; %u under "
		    "64 KB, %u under 1 MB, %u under 8 MB, %u larger), %u kept ones given again (%llu MB kept)\n",
		    gGPUStat.made, gGPUStat.made ? making / NSEC_PER_USEC / gGPUStat.made : 0,
		    gGPUStat.made ? allocating / NSEC_PER_USEC / gGPUStat.made : 0,
		    gGPUStat.made ? locking / NSEC_PER_USEC / gGPUStat.made : 0, gGPUStat.made_by_size[0], gGPUStat.made_by_size[1],
		    gGPUStat.made_by_size[2], gGPUStat.made_by_size[3], gGPUStat.given_again, fKeptBytes >> 20);
		/* Three messages, one record each: what a window's submissions cost here, what the GPU did
		 * with them, and what they named. */
		IOLog("RedwoodGPU: %u submissions in %llu ms: %llu us each in the kernel, %llu of them waiting for room in the queue; "
		    "%u command buffers and %u buffers named, %llu KB out of the cache in %llu us a submission\n",
		    gGPUStat.submissions, nanos / NSEC_PER_MSEC, submitting / NSEC_PER_USEC / gGPUStat.submissions,
		    rooming / NSEC_PER_USEC / gGPUStat.submissions, gGPUStat.commands, gGPUStat.buffers,
		    (gGPUStat.flushed >> 10) / gGPUStat.submissions, flushing / NSEC_PER_USEC / gGPUStat.submissions);
		IOLog("RedwoodGPU: the GPU worked %llu us on each of %u (%u under 0.1 ms, %u under 0.25, %u under 0.5, "
		    "%u under 1, %u under 2, %u under 4, %u longer; %llu ms in all)\n",
		    timed ? worked / timed : 0, timed, by[0], by[1], by[2], by[3], by[4], by[5], by[6], worked / 1000);
		IOLog("RedwoodGPU: %u waits of %llu us, %u over before they began; %u IOSurfaces taken as buffers in %llu us each; "
		    "%u buffers, %llu MB\n",
		    gGPUStat.waits, gGPUStat.waits ? waiting / NSEC_PER_USEC / gGPUStat.waits : 0, gGPUStat.over,
		    gGPUStat.surfaces, gGPUStat.surfaces ? taking / NSEC_PER_USEC / gGPUStat.surfaces : 0, fBufferCount,
		    fBufferBytes >> 20);
		uint32_t mhz, changes;

		redwood_gpu_speed(&mhz, &changes);
		if (mhz != 0) {
			IOLog("RedwoodGPU: GPU at %u MHz, %u speed changes since it started\n", mhz, changes);
		}
		if (gpu_va != 0) {
			/*
			 * The address space itself, because nothing else said anything about it: what
			 * argument buffers and heaps are built on is that this allocator holds, and
			 * "lost" and "refused" are the two ways it stops holding (bsd/dev/
			 * redwood_adreno.c, redwood_gpu_address_free).
			 */
			uint64_t used, largest, lost;
			uint32_t live, refused;
			unsigned holes, most;

			redwood_gpu_space(&used, &live, &largest, &holes, &most, &lost, &refused);
			IOLog("RedwoodGPU: addresses: %llu MB in %u buffers, longest run free %llu MB, %u holes "
			    "(%u at most, of 1024), %llu KB lost, %u allocations refused\n", used >> 20, live,
			    largest >> 20, holes, most, lost >> 10, refused);
		}
		bzero(&gGPUStat, sizeof(gGPUStat));
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, request->nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, request->nr_cmds);
	}
	return ret;
}
