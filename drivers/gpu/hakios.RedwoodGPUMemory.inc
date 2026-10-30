/* The descriptor and IOVA remain owned until unmap and TLB invalidation succeed.
 * A failed backend may still DMA into any submitted buffer, even with no returned fence. */
static bool
gpuReleaseMapping(uint64_t address, uint64_t size)
{
	if (gGPUReported || !redwood_gpu_ready() || !redwood_gpu_unmap(address, size)) {
		gGPUReported = true;
		return false;
	}
	redwood_gpu_address_free(address, size);
	return true;
}

/* With the lock held. A pending buffer moves to fLeft; failed unmap retains its existing record. */
void
RedwoodGPUUserClient::freeBuffer(struct gpu_buffer * b)
{
	bool busy = gGPUReported || !redwood_gpu_ready() || (b->fence != 0 && !redwood_gpu_done(b->fence));

	if (busy && fLeftCount == GPU_LEFT && !gGPUReported && redwood_gpu_ready()) {
		busy = !gpuWait(b->fence, 1000);        /* (nowhere to keep it: this one is waited for) */
	}
	if (busy && fLeftCount < GPU_LEFT) {
		struct gpu_left * left = &fLeft[fLeftCount++];

		left->memory = b->memory;
		left->surface = b->surface;
		left->address = b->address;
		left->size = b->size;
		left->fence = b->fence;
		left->flags = b->flags;
	} else {
		if (!busy && b->surface == NULL && keep(b->memory, b->kernel, b->address, b->size, b->flags)) {
			b->memory = NULL;
		} else if (busy || !gpuReleaseMapping(b->address, b->size)) {
			gGPUReported = true;
			IOLog("RedwoodGPU: retaining %llu bytes until reset after unconfirmed completion or unmap\n", b->size);
			return;
		}
		if (b->memory != NULL) {
			b->memory->release();
		}
		if (b->surface != NULL) {
			MiniIOSurfaceDrop(b->surface);
		}
	}
	b->memory = NULL;
	b->surface = NULL;
	fBufferCount--;
	fBufferBytes -= b->size;
}

/* The buffers that were given back early and that the GPU is done with now: freed. With the lock held. */
void
RedwoodGPUUserClient::reap(void)
{
	for (uint32_t i = 0; i < fLeftCount;) {
		struct gpu_left * left = &fLeft[i];

		if (gGPUReported || !redwood_gpu_ready() || !redwood_gpu_done(left->fence)) {
			i++;
			continue;
		}
		if (left->surface == NULL && keep(left->memory, NULL, left->address, left->size, left->flags)) {
			left->memory = NULL;
		} else if (!gpuReleaseMapping(left->address, left->size)) {
			i++;
			continue;
		}
		if (left->memory != NULL) {
			left->memory->release();
		}
		if (left->surface != NULL) {
			MiniIOSurfaceDrop(left->surface);
		}
		*left = fLeft[--fLeftCount];
	}
}

IOReturn
RedwoodGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		/* what the client had the GPU do last may still be going on, in these buffers */
		if (redwood_gpu_ready()) {
			gpuWait(redwood_gpu_last(), 1000);
		}
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		reap();
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			dropKept(&fKept[i]);
		}
		if (fLeftCount != 0) {
			IOLog("RedwoodGPU: the GPU is not done with %u buffers of a program that is gone: they stay its own\n", fLeftCount);
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
RedwoodGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct gpu_buffer *
RedwoodGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		/* (a program that names a buffer it has given back draws into somebody else's next: worth knowing) */
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("RedwoodGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * A handle and addresses of the GPU's for `size` bytes of a memory descriptor, from byte `from`
 * of it on (wired memory; both in whole pages). With the lock held; NULL: allocation or mapping failed.
 */
struct gpu_buffer *
RedwoodGPUUserClient::newBuffer(IOMemoryDescriptor * memory, uint64_t from, uint64_t size, uint32_t flags, uint32_t * handle)
{
	struct gpu_buffer * b = NULL;
	bool mapped = true;

	if (gGPUReported || !redwood_gpu_ready() || size == 0 || from > memory->getLength() ||
	    size > memory->getLength() - from || ((from | size) & 4095) != 0) {
		return NULL;
	}
	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = redwood_gpu_address(size)) == 0) {
		IOLog("RedwoodGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	/* one entry for each 4 KB, one lookup for each physically contiguous run of them */
	addr64_t phys = 0;
	IOByteCount length = 0;

	for (uint64_t offset = 0; offset < size && mapped; offset += 4096, phys += 4096, length -= 4096) {
		if (length < 4096) {
			phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);
			mapped = phys != 0 && (phys & 4095) == 0 && length >= 4096;
		}
		mapped = mapped && redwood_gpu_map(b->address + offset, phys, flags & MSM_BO_GPU_READONLY);
	}
	mapped = redwood_gpu_mapped() && mapped;
	if (!mapped) {
		if (!gpuReleaseMapping(b->address, size)) {
			/* The caller releases its reference on failure; this record retains the backing pages. */
			memory->retain();
			b->memory = memory;
			b->surface = NULL;
			b->size = size;
			b->flags = flags;
			b->fence = 0;
			fBufferCount++;
			fBufferBytes += size;
		}
		IOLog("RedwoodGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->size = size;
	b->flags = flags;
	b->seed = 0;
	b->pushed = 0;
	b->seen = false;
	b->fence = 0;
	b->surface = NULL;
	fBufferCount++;
	fBufferBytes += size;
	return b;
}

/* With the lock held. */
bool
RedwoodGPUUserClient::dropKept(struct gpu_kept * k)
{
	if (k->memory == NULL) {
		return true;
	}
	if (!gpuReleaseMapping(k->address, k->size)) {
		return false;
	}
	k->memory->release();
	k->memory = NULL;
	fKeptBytes -= k->size;
	return true;
}

/* With the lock held, the GPU done with the buffer: kept if it is worth it (those kept longest make way). */
bool
RedwoodGPUUserClient::keep(IOMemoryDescriptor * memory, uint8_t * kernel, uint64_t address, uint64_t size, uint32_t flags)
{
	struct gpu_kept * place = NULL;

	if (gGPUKeep == 0 || size < GPU_KEEP_FROM || size > GPU_KEPT_BYTES / 4 || memory == NULL ||
	    (kernel == NULL && OSDynamicCast(IOBufferMemoryDescriptor, memory) == NULL)) {
		return false;
	}
	for (;;) {
		struct gpu_kept * oldest = NULL;

		place = NULL;
		for (uint32_t i = 0; i < GPU_KEPT; i++) {
			if (fKept[i].memory == NULL) {
				place = place != NULL ? place : &fKept[i];
			} else if (oldest == NULL || fKept[i].since < oldest->since) {
				oldest = &fKept[i];
			}
		}
		if (place != NULL && fKeptBytes + size <= GPU_KEPT_BYTES) {
			break;
		}
		if (!dropKept(oldest)) {
			return false;
		}
	}
	place->memory = memory;
	place->kernel = kernel != NULL ? kernel : (uint8_t *) OSDynamicCast(IOBufferMemoryDescriptor, memory)->getBytesNoCopy();
	place->address = address;
	place->size = size;
	place->flags = flags;
	place->since = mach_absolute_time();
	fKeptBytes += size;
	return true;
}

IOReturn
RedwoodGPUUserClient::gemNew(struct msm_gem_new * request)
{
	uint64_t size = round_page_64(request->size), began = mach_absolute_time();
	struct gpu_buffer * b;
	IOBufferMemoryDescriptor * memory;

	if (size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* one kept of this size and kind, if there is one (the one kept last: likeliest still in the caches' way) */
	IOLockLock(gGPULock);
	if (gGPUReported || !redwood_gpu_ready()) {
		IOLockUnlock(gGPULock);
		return kIOReturnNotReady;
	}
	struct gpu_kept * found = NULL;
	uint64_t old;

	nanoseconds_to_absolutetime(GPU_KEPT_SECONDS * NSEC_PER_SEC, &old);
	for (uint32_t i = 0; i < GPU_KEPT; i++) {
		struct gpu_kept * k = &fKept[i];

		if (k->memory != NULL && began - k->since > old && !dropKept(k)) {
			IOLockUnlock(gGPULock);
			return kIOReturnNotReady;
		}

		if (k->memory != NULL && k->size == size && k->flags == request->flags && (found == NULL || k->since > found->since)) {
			found = k;
		}
	}
	for (uint32_t n = 0; found != NULL && n < GPU_BUFFERS; n++) {
		uint32_t h = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[h].memory == NULL) {
			b = &fBuffers[h];
			bzero(b, sizeof(*b));
			b->memory = found->memory;
			b->kernel = found->kernel;
			b->address = found->address;
			b->size = found->size;
			b->flags = found->flags;
			fNextBuffer = h + 1;
			fBufferCount++;
			fBufferBytes += size;
			request->handle = h + 1;
			found->memory = NULL;
			fKeptBytes -= size;
			gGPUStat.given_again++;
			IOLockUnlock(gGPULock);
			if (gGPUKeep == 2) {
				bzero(b->kernel, (size_t) b->size);
			}
			return kIOReturnSuccess;
		}
	}
	IOLockUnlock(gGPULock);
	/* (the memory comes zeroed, and making it write-combined pushes the zeroes out of the cache) */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	    (gGPUCached ? 0 : kIOMapWriteCombineCache), (vm_size_t) size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("RedwoodGPU: no memory for a buffer of %llu bytes (%u buffers, %llu MB with this client)\n", size, fBufferCount,
		    fBufferBytes >> 20);
		return kIOReturnNoMemory;
	}
	if (gGPUCached) {
		redwood_gpu_to_memory(memory->getBytesNoCopy(), size);
	}
	uint64_t allocated = mach_absolute_time();

	IOLockLock(gGPULock);
	gGPUStat.locking += mach_absolute_time() - allocated;
	b = newBuffer(memory, 0, size, request->flags, &request->handle);
	if (b != NULL) {
		b->memory = memory;
		b->kernel = (uint8_t *) memory->getBytesNoCopy();
		gGPUStat.made++;
		gGPUStat.making += mach_absolute_time() - began;
		gGPUStat.allocating += allocated - began;
		gGPUStat.made_by_size[size < (64u << 10) ? 0 : size < (1u << 20) ? 1 : size < (8u << 20) ? 2 : 3]++;
	}
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer: its pixels, from the page after the surface's shared
 * state on (MiniIOSurface.h). The surface is held for as long as the buffer is there. A surface
 * that is a buffer of this client's already is that buffer again, as with Linux's handles: so
 * the client must not be giving that buffer back at the same moment (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe (kRedwoodDescribe), no buffer is made.
 */
IOReturn
RedwoodGPUUserClient::surfaceBuffer(struct redwood_surface * request)
{
	MiniIOSurface * s = MiniIOSurfaceCopy(request->surface);
	struct gpu_buffer * b = NULL;
	IOMemoryDescriptor * memory;
	uint64_t size, began = mach_absolute_time();

	if (s == NULL) {
		return kIOReturnNotFound;
	}
	if (s->client != NULL) {
		/* (memory of a program's own with a surface made around it: not in whole pages, and seldom in rows the GPU takes) */
		MiniIOSurfaceDrop(s);
		return kIOReturnUnsupported;
	}
	size = round_page_64(s->info.allocSize);
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, size, kIODirectionInOut);
	if (memory == NULL || size == 0 || size > GPU_BUFFER_MAX) {
		OSSafeReleaseNULL(memory);
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kRedwoodDescribe) {
		request->handle = 0;
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	if (gGPUReported || !redwood_gpu_ready()) {
		IOLockUnlock(gGPULock);
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNotReady;
	}
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
	} else if ((b = newBuffer(s->memory, PAGE_SIZE, size, 0, &request->handle)) != NULL) {
		b->memory = memory;
		b->kernel = s->kbase + PAGE_SIZE;
		b->surface = s;
		gGPUStat.surfaces++;
	}
	gGPUStat.taking += mach_absolute_time() - began;
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		MiniIOSurfaceDrop(s);
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}
