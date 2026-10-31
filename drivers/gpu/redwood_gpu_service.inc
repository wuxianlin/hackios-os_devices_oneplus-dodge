#include "redwood_gpu_private.h"

/* ---- the GPU kept on, for the system to draw with --------------------------------------------------- */

/*
 * With boot-arg adreno_keep=1 the steps are not undone: the GPU block stays as step 21 leaves it,
 * the 3D core powered and taking commands, and the service RedwoodGPU (iokit/Kernel/RedwoodGPU.cpp)
 * hands it to programs through the interface of Linux's msm driver, which is what Mesa's
 * freedreno speaks. That file keeps the buffers and the clients; what is the GPU's is here:
 * addresses in the 3D core's SMMU context, the ring, and the word by which the core says how
 * far it has got (a "fence": a submission's number, written by the core when it is through
 * with it). The caller holds one lock around all of these.
 *
 * Boot-arg gpu_recover=1: a submission the core never finishes does not cost the rest of the
 * run. The core's own half is reset and started again, and what was outstanding is given up
 * (legacy_gpu_recover() below, which says what is done and which source says so). Without it
 * such a submission is only reported, and the GPU stays stopped.
 */
#define GPU_CHIP_ID             0x06030500ull   /* dt: qcom,chipid */
#define GPU_ADDRESSES_START     0x20000000ull   /* above everything the steps use; below 4 GB, as Linux keeps them */
#define GPU_ADDRESSES_SIZE      0xe0000000ull
#define GPU_FENCE_AT            0x40            /* in the scratch page */
#define GPU_TIMES_AT            0x400           /* ... and when the core began and ended each of its last submissions */
#define GPU_TIMES               64
#define CP_ALWAYS_ON_COUNTER    0x0980          /* ticks of the 19.2 MHz crystal, 64 bits: for the core to read itself */
#define CP_REG_TO_MEM           0x3e
#define CP_SCRATCH_REG2         0x0885
#define CP_IB1_BASE             GPU_WORD(0x0928)
#define CP_IB1_REM_SIZE         GPU_WORD(0x092a)
#define CP_IB2_BASE             GPU_WORD(0x092b)
#define CP_IB2_REM_SIZE         GPU_WORD(0x092d)
#define CP_INDIRECT_BUFFER      0x3f
#define EVENT_INVALIDATE_DEPTH  0x18
#define EVENT_INVALIDATE_COLOR  0x19
/* and, for a core that has stopped answering (gpu_recover=1: legacy_gpu_recover() below) */
#define RBBM_SW_RESET_CMD       GPU_WORD(0x0043)        /* a6xx.xml; 1 holds the 3D core's own half (GX) in reset */
#define RBBM_INT_CLEAR_CMD      GPU_WORD(0x0037)        /* the bits of rbbm_int_0_status written here are forgotten */
#define GMU_PWR_COL_KEEPALIVE   GMU_WORD(0x50c3)        /* a6xx_gmu.xml; 1: the GMU keeps the core powered, whatever it makes of it */
#define GMU_AHB_FENCE_STATUS_CLR GMU_WORD(0x9314)       /* the bits of ahb_fence_status written here are forgotten */
#define GPU_RECOVERIES          64                      /* at most in one run: a program that hangs every submission fills no log with this */

static bool gpu_kept;
static uint32_t gpu_fence;              /* the number of the last submission */
static uint32_t gpu_timed;              /* ... of the last whose times have been added up */
static uint64_t gpu_ticks;              /* how long the core has worked on the submissions added up */
static uint32_t gpu_timed_count;
static uint32_t gpu_timed_by[7];        /* ... how many of them under 0.1, 0.25, 0.5, 1, 2, 4 ms, and longer */
static uint32_t gpu_exact = 1;          /* boot-arg gpu_exact=0: the core does not wait to be idle before it notes the time */
static uint32_t gpu_recover;            /* boot-arg gpu_recover=1: a core that has stopped answering is reset and given work again */
static uint32_t gpu_recoveries;         /* ... and how many times that has been done in this run */
uint32_t gpu_log;                       /* boot-arg gpu_log=1: a line for every submission and the time the core spent on it */
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
static struct {
	uint64_t start, size;
} gpu_free[1024];                       /* the addresses not in use, in order */
static unsigned gpu_free_count;

/*
 * ---- the 3D core's addresses, and who owns them -------------------------------------------
 *
 * There is one address space for everything the 3D core reads and writes: context 0 of the
 * GPU's SMMU, 39 bits of it, three levels of 4K pages, made by step 18 (core_space above).
 * Below GPU_ADDRESSES_START the steps put their own blocks at constants of their own -- the
 * ring at 0x01000000, the microcode at 0x01100000, the scratch page at 0x01200000, the test's
 * target at 0x10000000 -- and those are constants and not allocations: there is one of each
 * for the whole run. From GPU_ADDRESSES_START up the addresses are handed out a buffer at a
 * time by legacy_gpu_address() out of gpu_free, which is this driver's whole allocator: a
 * list of what is not in use, lowest first, first fit, coalesced when given back.
 *
 * That division of labour is Linux's msm, not an invention here. There the kernel picks an
 * address out of a drm_mm when a buffer is first asked about and keeps it in the object until
 * the object goes (msm_gem_get_iova -> get_vma_locked), and userspace only asks:
 * MSM_INFO_GET_IOVA. Mesa's freedreno does exactly that and nothing else -- fd_bo_init_common
 * sets bo->iova from msm_bo_iova(), one GEM_INFO call per buffer -- so the kernel is the
 * allocator for both of this port's consumers and neither of them fights it.
 *
 * One thing to know before anything is built on these addresses: there is one space, so an
 * address is the same memory in every program. That is what makes an argument buffer possible
 * at all -- a buffer of addresses means something wherever it is read -- and it is also why
 * one program's shader can reach another's buffers by naming an address it was never given.
 * Nothing here can check an address a shader computes; only page tables of the client's own
 * could, and there is one context for everybody (space_on() above, context 0).
 *
 * boot-arg gpu_va:
 *   0 (the default)  as it has always been. Addresses packed one allocation after another,
 *                    and VA_START/VA_SIZE not answered.
 *   1                VA_START and VA_SIZE answered, and the space's state printed with the
 *                    other GPU numbers. Not one address changes.
 *   2               ... and a granule of addresses left unmapped after every buffer, so that a
 *                    read or a write that runs off the end of one buffer falls into nothing and
 *                    the SMMU says so with the address (space_faulted()) instead of landing
 *                    silently in whatever buffer was made next. This is for argument buffers
 *                    above all: there the GPU dereferences an address that a program wrote into
 *                    memory, and a wrong address is otherwise indistinguishable from wrong data.
 *                    Every address moves, so it is not the default.
 *
 * Why VA_START and VA_SIZE are behind the arg rather than always answered: in msm they mean
 * "this space is yours to place buffers in", and msm refuses them outright to a client that
 * does not have page tables of its own (adreno_gpu.c, adreno_get_param: -EINVAL when
 * ctx->aspace == gpu->aspace). Here every client is in the one context 0, so the honest
 * default is msm's refusal; with the arg they are answered as what they are here, which is
 * the range the kernel allocates out of -- a number to size a heap or maxBufferLength against,
 * not a licence to place anything. Nothing in Mesa is affected either way: freedreno asks
 * VA_SIZE only at drm version 9 and up (FD_VERSION_VA_SIZE), and tools/mesa/redwood-drm says 1.7.
 */
uint32_t gpu_va;                        /* boot-arg gpu_va: 0 as before, 1 describe, 2 also guard */
#define GPU_ADDRESS_GRANULE     0x10000ull      /* what one allocation is rounded up to */
static uint64_t gpu_va_used;            /* addresses handed out and not given back */
static uint32_t gpu_va_live;            /* ... to how many buffers */
static unsigned gpu_va_holes;           /* the most entries of gpu_free ever in use at once */
static uint64_t gpu_va_lost;            /* given back, and gpu_free had no room to remember it */
static uint32_t gpu_va_refused;         /* allocations that found no run long enough */

/*
 * How many addresses a buffer of so many bytes takes. Both ends of the allocator use this, so
 * the guard is part of the allocation and not a hole left between two: a buffer given back
 * returns its guard with it and the free list coalesces exactly as it did without the arg.
 * A hole per live buffer would instead fill the 1024 entries and start losing addresses.
 */
static uint64_t
gpu_address_span(uint64_t size)
{
	uint64_t span = (size + (GPU_ADDRESS_GRANULE - 1)) & ~(GPU_ADDRESS_GRANULE - 1);

	return gpu_va >= 2 ? span + GPU_ADDRESS_GRANULE : span;
}

extern bool RedwoodGPUStart(void);
extern void RedwoodGPUStop(void);       /* takes the clients' lock, then calls redwood_gpu_stop() */
static void gpu_block_stop(void);

/*
 * A run that ends in order -- its time is up, the host asks for the end, five fingers on the
 * screen, a halt: whoever is about to call loader_return() for one of these calls
 * redwood_adreno_stop() first, from a thread that may wait -- gives the GPU back as the steps'
 * own ending does, and the next kernel finds the GPU block as the phone's start leaves it. A
 * panic cannot: it leaves the GPU block running but quiet (redwood_adreno_quiesce()), and the
 * next kernel's step 4 takes it down (stale_stop()). Boot-arg adreno_dirty=1 makes every ending
 * a panic's in this respect, to see that work.
 */
void
legacy_gpu_stop(void)
{
	if (gpu_kept) {
		uint32_t waited = 0;

		gpu_kept = false;                       /* (nothing more is taken) */
		quiet = false;
		/*
		 * What the core has been given, it finishes first: told to slumber in the middle of a
		 * frame, the GMU may not answer. Half a second at the most; then its bus is halted
		 * for good, and its power switched off from here.
		 */
		while (!legacy_gpu_done(gpu_fence) && waited < 5000) {
			sleep_us(100);
			waited++;
		}
		core_stuck = !legacy_gpu_done(gpu_fence);
		printf("adreno: the run is about to end: the GPU is given back (%s after %u us)\n",
		    core_stuck ? "STILL AT WORK" : "it had finished what it was given", 100 * waited);
		gpu_block_stop();
	}
}

void
redwood_adreno_stop(void)
{
	if (redwood_platform_is_annibale()) {
		RedwoodGPUStop();
		return;
	}
	uint32_t dirty = 0;

	if (gpu_kept && !(PE_parse_boot_argn("adreno_dirty", &dirty, sizeof(dirty)) && dirty)) {
		RedwoodGPUStop();
	}
}

bool
legacy_gpu_ready(void)
{
	return gpu_kept;
}

/* The answers to msm's GET_PARAM (Linux's adreno_get_param for this GPU; the numbers are msm_drm.h's). */
bool
legacy_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID: none, the chip ID says it */
	case 0x06:                              /* GMEM_BASE */
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING */
		*value = 0;
		return true;
	case 0x02:                              /* GMEM_SIZE */
		*value = 512 * 1024;
		return true;
	case 0x03:                              /* CHIP_ID, with the speed bin above it */
		*value = GPU_CHIP_ID | (uint64_t)speed_bin << 32;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = core_khz * 1000ull;
		return true;
	case 0x05:                              /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT */
		*value = 14;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE */
		*value = 6;
		return true;
	case 0x13:                              /* MACROTILE_MODE */
		*value = 1;
		return true;
	case 0x0e:                              /* VA_START */
	case 0x0f:                              /* VA_SIZE */
		/*
		 * Addresses are ours to give, so without gpu_va these are refused, which is what
		 * Linux's msm does for a client that shares the GPU's own page tables. With the
		 * arg they say what the range the kernel allocates out of is -- see gpu_va above.
		 */
		if (gpu_va == 0) {
			return false;
		}
		*value = which == 0x0e ? GPU_ADDRESSES_START : GPU_ADDRESSES_SIZE;
		return true;
	default:
		return false;
	}
}

/*
 * Addresses for a buffer of so many bytes; 0: none left. The address is the buffer's from here
 * until legacy_gpu_address_free(): nothing moves a live buffer, which is what userspace is
 * promised when it asks MSM_INFO_GET_IOVA once and keeps the answer (as freedreno does).
 */
uint64_t
legacy_gpu_address(uint64_t size)
{
	size = gpu_address_span(size);
	for (unsigned i = 0; i < gpu_free_count; i++) {
		if (gpu_free[i].size >= size) {
			uint64_t address = gpu_free[i].start;

			gpu_free[i].start += size;
			gpu_free[i].size -= size;
			if (gpu_free[i].size == 0) {
				memmove(&gpu_free[i], &gpu_free[i + 1], (--gpu_free_count - i) * sizeof(gpu_free[0]));
			}
			gpu_va_used += size;
			gpu_va_live++;
			return address;
		}
	}
	gpu_va_refused++;
	return 0;
}

void
legacy_gpu_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = gpu_address_span(size);
	gpu_va_used -= size;
	gpu_va_live--;
	for (at = 0; at < gpu_free_count && gpu_free[at].start < address; at++) {
	}
	if (at > 0 && gpu_free[at - 1].start + gpu_free[at - 1].size == address) {
		gpu_free[at - 1].size += size;
		if (at < gpu_free_count && address + size == gpu_free[at].start) {
			gpu_free[at - 1].size += gpu_free[at].size;
			memmove(&gpu_free[at], &gpu_free[at + 1], (--gpu_free_count - at) * sizeof(gpu_free[0]));
		}
	} else if (at < gpu_free_count && address + size == gpu_free[at].start) {
		gpu_free[at].start = address;
		gpu_free[at].size += size;
	} else if (gpu_free_count < sizeof(gpu_free) / sizeof(gpu_free[0])) {
		memmove(&gpu_free[at + 1], &gpu_free[at], (gpu_free_count++ - at) * sizeof(gpu_free[0]));
		gpu_free[at].start = address;
		gpu_free[at].size = size;
	} else {
		/*
		 * These addresses are lost: there is no room to remember the hole, and handing
		 * them out again without remembering would hand out somebody's live buffer. It
		 * is counted rather than passed over in silence, because a program that makes
		 * and frees buffers in an order that leaves 1024 holes loses address space
		 * steadily and nothing else would say so.
		 */
		gpu_va_lost += size;
	}
	if (gpu_free_count > gpu_va_holes) {
		gpu_va_holes = gpu_free_count;
	}
}

/*
 * What the 3D core's address space holds, for whoever prints the GPU's numbers (gpu_va=1).
 * `largest` is the longest run of addresses still free -- what the biggest buffer that can
 * still be made is bounded by -- and `most` is the most holes there have ever been at once,
 * which is the distance to the one way this allocator fails: at 1024 it starts losing them.
 */
void
legacy_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most, uint64_t *lost,
    uint32_t *refused)
{
	*largest = 0;
	for (unsigned i = 0; i < gpu_free_count; i++) {
		if (gpu_free[i].size > *largest) {
			*largest = gpu_free[i].size;
		}
	}
	*used = gpu_va_used;
	*live = gpu_va_live;
	*holes = gpu_free_count;
	*most = gpu_va_holes;
	*lost = gpu_va_lost;
	*refused = gpu_va_refused;
}

/* One 4K page of ours at an address of the 3D core's; after the last of a buffer's, legacy_gpu_mapped(). */
bool
legacy_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

bool
legacy_gpu_mapped(void)
{
	uint32_t context = smmu_context(&core_space);

	space_commit(&core_space);
	settle();
	wr(smmu.base + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu.base + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
	settle();
	return !(rd(smmu.base + context + SMMU_CB_TLBSTATUS) & 1);
}

bool
legacy_gpu_unmap(uint64_t address, uint64_t size)
{
	bool cleared = true;
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		cleared &= space_map(&core_space, address + offset, 0, 0);
	}
	bool synced = legacy_gpu_mapped();
	return cleared && synced;
}

/* Memory that the 3D core and a program see without this processor's cache in between: out of that cache. */
void
legacy_gpu_to_memory(void *start, uint64_t size)
{
	to_memory((vm_offset_t)start, size);
}

/* How far the 3D core has got. */
bool
legacy_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
legacy_gpu_last(void)
{
	return gpu_fence;
}

/* The core is to write down what time it is: at one end of submission `fence`. */
static void
ring_time(uint32_t fence, uint32_t end)
{
	ring_command(CP_REG_TO_MEM, 3);
	ring(CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);       /* two words, a 64-bit register */
	ring(CORE_SCRATCH_ADDRESS + GPU_TIMES_AT + 16 * (fence % GPU_TIMES) + 8 * end);
	ring(0);
}

/* Add up the times of the submissions the core has finished since this was last done. */
static void
gpu_times(void)
{
	volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + GPU_TIMES_AT);
	uint32_t done;

	to_memory(core_scratch.memory, 4096);
	done = *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT);
	if (done - gpu_timed > GPU_TIMES) {
		gpu_timed = done - GPU_TIMES;           /* (the older ones have been written over) */
	}
	while (gpu_timed != done) {
		uint32_t slot = ++gpu_timed % GPU_TIMES;

		if (gpu_timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
			static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };     /* (19.2 ticks a microsecond) */
			uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
			unsigned by = 0;

			if (gpu_log) {
				/* "tl: q FENCE took US": what the core spent on that submission (iokit/Kernel/RedwoodGPU.cpp: tl: Q) */
				redwood_tl("tl: q %u took %llu\n", gpu_timed, ticks * 10 / 192);
			}

			while (by < 6 && ticks >= under[by]) {
				by++;
			}
			gpu_timed_by[by]++;
			gpu_ticks += ticks;
			gpu_timed_count++;
		}
	}
}

/*
 * How long the 3D core has worked since this was last asked, on how many submissions, and how
 * many of those took it under 0.1, 0.25, 0.5, 1, 2, 4 milliseconds and longer.
 */
void
legacy_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	gpu_times();
	*microseconds = gpu_ticks * 10 / 192;
	*submissions = gpu_timed_count;
	memcpy(by, gpu_timed_by, sizeof(gpu_timed_by));
	bzero(gpu_timed_by, sizeof(gpu_timed_by));
	gpu_ticks = 0;
	gpu_timed_count = 0;
}

/*
 * Command buffers for the 3D core to run, in order: each an address and a count of words. As
 * Linux's a6xx_submit puts them in the ring. Returns the submission's number; 0: the ring is
 * full and the core is not reading it.
 */
uint32_t
legacy_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 26 + 4 * count, room = 0;

	for (int i = 0; i < 20000; i++) {
		room = (rd(gpu.base + CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		sleep_us(100);
	}
	if (room < wanted) {
		return 0;
	}
	gpu_times();
	if (++gpu_fence == 0) {
		gpu_fence = 1;
	}
	ring_time(gpu_fence, 0);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE, 1);
	ring(EVENT_INVALIDATE_COLOR);
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	if (gpu_exact) {
		ring_command(CP_WAIT_FOR_IDLE, 0);      /* the time is the pipeline's, not the command processor's, which is ahead of it */
	}
	ring_time(gpu_fence, 1);
	ring_register(CP_SCRATCH_REG2, gpu_fence);
	ring_command(CP_EVENT_WRITE, 4);
	ring(EVENT_CACHE_FLUSH);
	ring(CORE_SCRATCH_ADDRESS + GPU_FENCE_AT);
	ring(0);
	ring(gpu_fence);
	ring_send();
	return gpu_fence;
}

/* A submission does not come back: what the 3D core and its SMMU context say. */
void
legacy_gpu_report(const char *why)
{
	uint32_t context = smmu_context(&core_space);

	printf("adreno: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u, written up to %u; status "
	    "%#x, interrupts %#x; command processor: fault %#x, interrupts %#x, protection %#x\n", why, gpu_fence,
	    *(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT), rd(gpu.base + CP_RB_RPTR), ring_at,
	    rd(gpu.base + RBBM_STATUS), rd(gpu.base + RBBM_INT_0_STATUS), rd(gpu.base + CP_HW_FAULT),
	    rd(gpu.base + CP_INTERRUPT_STATUS), rd(gpu.base + CP_PROTECT_STATUS));
	printf("adreno: gpu:   in the first command buffer at %#llx, %u words to go; in the second at %#llx, %u to go\n",
	    (uint64_t)rd(gpu.base + CP_IB1_BASE + 4) << 32 | rd(gpu.base + CP_IB1_BASE), rd(gpu.base + CP_IB1_REM_SIZE),
	    (uint64_t)rd(gpu.base + CP_IB2_BASE + 4) << 32 | rd(gpu.base + CP_IB2_BASE), rd(gpu.base + CP_IB2_REM_SIZE));
	step = 0;
	if (space_faulted(&core_space)) {
		wr(smmu.base + context + SMMU_CB_FSR, SMMU_FSR_ALL);    /* so that the next one shows */
	}
}

/*
 * boot-arg gpu_recover=1: the 3D core is put back where step 20 and step 21 left it, and
 * everything it had been given is given up, so that the next submission is its own again.
 * Returns whether that worked; without the boot-arg, false at once and nothing is touched.
 *
 * Why it is worth doing: one submission the core never finishes otherwise costs the rest of the
 * run. gpuWait() (iokit/Kernel/RedwoodGPU.cpp) stops waiting for good after the first such
 * submission, and from then on every wait says at once that the GPU is not done -- so every
 * later submission "succeeds" and writes nothing, which reads like a measurement and is not
 * one. With this, a core that hangs costs one submission.
 *
 * What is done, and who says so (Linux, drivers/gpu/drm/msm/adreno/a6xx_gpu.c unless said
 * otherwise):
 *
 *   - the command processor is halted first, so that nothing more of the ring is fetched while
 *     the rest of this happens (a6xx_recover: "Halt SQE first", cp_sqe_cntl = 3; step 20 starts
 *     it with 1, and bit 0 is what runs it);
 *   - the GMU is told to keep the core powered whatever it makes of what follows (Linux sets
 *     the same word when its hang interrupt is about to look at a core that has stopped:
 *     a6xx_fault_detect_irq. Nothing has set it here, there being no interrupt of ours);
 *   - what the core has under way on the bus is let finish and nothing new is taken
 *     (a6xx_bus_clear_pending_transactions with gx_off, which a6xx_recover calls before it
 *     pulls the reset line: core_bus_halt(true) here, which also halts the core's own side);
 *   - the reset line of the core's own half (GX) is pulled and let go (a6xx_gpu_sw_reset, whose
 *     timings and read in between these are: the line is held for 100 us). Everything that was
 *     wedged is in that half -- the shader cores, the depth and colour pipeline, the caches,
 *     the command processor. The CX half is not reset, so the SMMU keeps its tables and the
 *     core's memory stays where it was mapped; nothing of ours has to be made again;
 *   - the ring is put back to where the core says it reads from, with nothing to read;
 *   - and the core is started again exactly as the steps start it: step 20's registers, its
 *     command processor, "initialize"; then step 21, out of secure mode and a word written to
 *     memory on command, which is that step's own proof that the core does as it is told. The
 *     zap shader stays where TrustZone put it -- Linux does not load it again either
 *     (a6xx_zap_shader_init keeps a flag) and asks the core to leave secure mode after every
 *     reset just the same.
 *
 * Not Linux's, and the reason this is behind a boot-arg: Linux never lets go of that reset line
 * on a part whose GMU runs firmware of its own. a6xx_recover() does the reset only for the a6xx
 * parts whose "GMU" is a wrapper with no firmware; on a part like this one it takes the whole
 * CX half's power away instead and starts from nothing, because its GMU is set up afresh every
 * time the GPU is woken. The one place Linux resets GX on a part with a real GMU is
 * a6xx_gmu_force_off() (a6xx_gmu.c), and there it pulls the line and leaves it pulled -- the CX
 * half is about to lose power anyway. Here the GMU is set up once, by steps 15 to 17, and never
 * spoken to again, so there is nothing of its state that a submission depends on; but whether a
 * GMU that was told none of this goes on believing it owns a core that has just been reset is a
 * question the phone answers, not the sources. So: with the boot-arg left out, none of this
 * runs and a wedge costs the run, exactly as before.
 *
 * Steps 20 and 21 are called as they are rather than copied, so that there is one list of the
 * core's registers and not two. That costs their two cx_ready() calls, which measure the
 * register bus's clock (some 28 ms each) and leave `quiet` false, so the recovery says every
 * register it writes. Once per hang, that is evidence rather than noise.
 */
bool
legacy_gpu_recover(void)
{
	uint32_t fence_status;

	if (!gpu_kept || !gpu_recover) {
		return false;
	}
	if (gpu_recoveries >= GPU_RECOVERIES) {
		printf("adreno: gpu: the 3D core has been recovered %u times in this run already: no more\n", gpu_recoveries);
		return false;
	}
	gpu_recoveries++;
	step = 0;                               /* (the steps are over: no line of theirs waits for the loader any more) */
	newest_step = ~0u;

	wr(gpu.base + CP_SQE_CNTL, 3);
	wr(gmu.base + GMU_PWR_COL_KEEPALIVE, 1);
	core_bus_halt(true);
	wr(gpu.base + RBBM_SW_RESET_CMD, 1);
	rd(gpu.base + RBBM_SW_RESET_CMD);       /* (Linux reads it back and waits, to be sure of the order) */
	sleep_us(101);
	wr(gpu.base + RBBM_SW_RESET_CMD, 0);
	rd(gpu.base + RBBM_SW_RESET_CMD);
	sleep_us(1);
	/*
	 * Whatever the GMU dropped of writes of ours while the core was wedged is forgotten, and its
	 * fence is put back where it lets them through -- Linux does both before it resets a core
	 * (a6xx_gmu_force_off, a6xx_gmu.c; nothing here has ever written the fence's control word, so
	 * zero is what it already holds). A fence gone to dropping writes would swallow every
	 * register step 20 writes below, silently, and the failure would then read as a core that
	 * does not start rather than as writes that never arrived. Hence also the status word in the
	 * line below: its bits are exactly those writes.
	 */
	fence_status = rd(gmu.base + GMU_AHB_FENCE_STATUS);
	wr(gmu.base + GMU_AHB_FENCE_STATUS_CLR, 7);
	wr(gmu.base + GMU_AO_AHB_FENCE_CTRL, 0);
	/*
	 * Where the ring stands is the core's word for it: a position left over from before would
	 * have it fetch words it has run already, or words of a submission that is being given up.
	 * The two positions are made equal, whatever it says, so there is nothing to fetch until
	 * something is submitted; step 20 then writes the ring's address and size again itself.
	 */
	ring_at = ring_sent = rd(gpu.base + CP_RB_RPTR) % RING_WORDS;
	wr(gpu.base + CP_RB_WPTR, ring_at);
	wr(gpu.base + RBBM_INT_CLEAR_CMD, rd(gpu.base + RBBM_INT_0_STATUS));    /* so that what is reported next is this core's */
	printf("adreno: gpu: the 3D core's half has been reset: status %#x, interrupts %#x, ring at %u; the GMU sees it busy: "
	    "%#x, and powered: %#x (bits 6 and 7 clear); it had dropped writes of ours: %#x\n", rd(gpu.base + RBBM_STATUS),
	    rd(gpu.base + RBBM_INT_0_STATUS), ring_at, rd(gmu.base + GMU_AO_GPU_CX_BUSY_STATUS),
	    rd(gmu.base + GMU_SPTPRAC_PWR_CLK_STATUS), fence_status);

	if (!step_core_start() || !step_core_commands()) {
		printf("adreno: gpu: THE 3D CORE DOES NOT TAKE COMMANDS AGAIN after the reset: it is given up for this run, as it "
		    "would have been without gpu_recover=1\n");
		wr(gmu.base + GMU_PWR_COL_KEEPALIVE, 0);
		quiet = true;
		return false;
	}
	wr(gmu.base + GMU_PWR_COL_KEEPALIVE, 0);
	quiet = true;                           /* (cx_ready() measures a clock, and measure_quietly() ends by clearing this) */

	/*
	 * And everything the core had been given is given up: the word by which it says how far it
	 * has got is set to the last submission's number, so that every wait outstanding ends. The
	 * submission that hung, and whatever was behind it in the ring, did not run and now never
	 * will; calling them done is what Linux does with the ring's fence when it recovers
	 * (msm_gpu.c, recover_worker: "advance the fence by one more to clear the faulting
	 * submit"), and it is the only answer that lets the waiters go. Their times go with them:
	 * a submission that did not run is not counted among those the core worked on.
	 */
	bzero((void *)core_scratch.memory, 4096);
	*(volatile uint32_t *)(core_scratch.memory + GPU_FENCE_AT) = gpu_fence;
	to_memory(core_scratch.memory, 4096);
	gpu_timed = gpu_fence;
	printf("adreno: gpu: THE 3D CORE IS RECOVERED (%u time%s in this run); submissions up to %u are given up as done\n",
	    gpu_recoveries, gpu_recoveries == 1 ? "" : "s", gpu_fence);
	return true;
}
