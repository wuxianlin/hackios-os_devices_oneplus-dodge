/* Existing RedwoodGPU C ABI, selected by the immutable loader platform description. */
#include "adreno8_private.h"
#include "adreno8_bo_private.h"
#include "adreno8_context_private.h"
#include "adreno8_hwsched_private.h"
#include "redwood_gpu_private.h"
#include <sys/hakios.input.h>

/* this port runs on the dodge with the adreno8 driver: the "annibale
 * platform" check of the K90 dispatch is always true here */
static bool
redwood_platform_is_annibale(void)
{
	return true;
}


extern bool RedwoodGPUStart(void);
extern uint32_t gpu_va;

bool
adreno8_step_service(void)
{
	if (!redwood_platform_is_annibale() || !adreno8_gpu8_keep || adreno8_gpu8_return) {
		adreno8_say("adreno8: RedwoodGPU requires the annibale platform, keep=1 and return=0\n");
		return false;
	}
	if (!adreno8_bo_enable()) {
		adreno8_say("adreno8: RedwoodGPU external BO enable was refused\n");
		return false;
	}
	gpu_va = 0; /* The shared address space has no per-client placement or BO-count query. */
	PE_parse_boot_argn("gpu_log", &gpu_log, sizeof(gpu_log));
	if (!RedwoodGPUStart()) {
		adreno8_say("adreno8: RedwoodGPU service registration failed\n");
		adreno8_bo_stop();
		return false;
	}
	adreno8_say("adreno8: RedwoodGPU registered with A830 BOs and standard HWSCHED submission\n");
	return true;
}

bool
redwood_gpu_ready(void)
{
	return redwood_platform_is_annibale() ? adreno8_bo_ready() : legacy_gpu_ready();
}

bool
redwood_gpu_parameter(uint32_t which, uint64_t *value)
{
	if (!redwood_platform_is_annibale()) return legacy_gpu_parameter(which, value);
	if (!value || !adreno8_bo_ready()) return false;
	switch (which) {
	case 0x01: *value = 830; break;                 /* GPU_ID */
	case 0x02: *value = 0xc00000; break;            /* GMEM_SIZE: vendor gen8_0_1 */
	case 0x03: *value = 0x44050001; break;          /* CHIP_ID: running Android DT */
	case 0x04: *value = adreno8_gx_hz[adreno8_gx_count - 1]; break; /* MAX_FREQ: the fastest speed of the table */
	case 0x06: *value = 0x04000000; break;          /* GMEM_BASE: GPU setup */
	case 0x07: *value = 1; break;                   /* PRIORITIES: RB0 only */
	case 0x08:                                    /* PP_PGTABLE */
	case 0x09:                                    /* FAULTS: unhealthy state is rejected above */
	case 0x0a:                                    /* SUSPENDS: no suspend transition */
	case 0x11: *value = 0; break;                  /* RAYTRACING: unavailable */
	case 0x10: *value = 16; break;                 /* HIGHEST_BANK_BIT: vendor gen8_0_1 */
	case 0x14: *value = 0x1fffffffff000ULL; break;  /* UCHE_TRAP_BASE: GPU setup */
	default: return false;
	}
	return true;
}

uint64_t
redwood_gpu_address(uint64_t size)
{
	return redwood_platform_is_annibale() ? adreno8_bo_reserve(size) : legacy_gpu_address(size);
}

void
redwood_gpu_address_free(uint64_t address, uint64_t size)
{
	if (redwood_platform_is_annibale()) {
		(void)adreno8_bo_release(address, size);
		return;
	}
	legacy_gpu_address_free(address, size);
}

void
redwood_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes,
    unsigned *most, uint64_t *lost, uint32_t *refused)
{
	if (!redwood_platform_is_annibale()) {
		legacy_gpu_space(used, live, largest, holes, most, lost, refused);
		return;
	}
	/* gpu_va stays disabled on A830: these legacy allocator statistics are unavailable. */
	*used = *largest = *lost = 0;
	*live = *holes = *most = *refused = 0;
}

bool
redwood_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return redwood_platform_is_annibale() ? adreno8_bo_map(address, phys, read_only) :
	    legacy_gpu_map(address, phys, read_only);
}

bool
redwood_gpu_mapped(void)
{
	return redwood_platform_is_annibale() ? adreno8_bo_commit() : legacy_gpu_mapped();
}

bool
redwood_gpu_unmap(uint64_t address, uint64_t size)
{
	return redwood_platform_is_annibale() ? adreno8_bo_unmap(address, size) : legacy_gpu_unmap(address, size);
}

void
redwood_gpu_to_memory(void *start, uint64_t size)
{
	if (redwood_platform_is_annibale()) {
		/* Initial cached-BO publication runs outside gGPULock; keep this cache-only. */
		adreno8_to_memory((vm_offset_t)start, size);
		return;
	}
	legacy_gpu_to_memory(start, size);
}

uint32_t
redwood_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	if (!redwood_platform_is_annibale()) return legacy_gpu_submit(addresses, words, count);
	if (!adreno8_bo_ready()) return 0;
	adreno8_speed_tick();
	return adreno8_context_submit(addresses, words, count);
}

bool
redwood_gpu_done(uint32_t fence)
{
	if (!redwood_platform_is_annibale()) return legacy_gpu_done(fence);
	return adreno8_bo_ready() && adreno8_context_done(fence);
}

uint32_t
redwood_gpu_last(void)
{
	return redwood_platform_is_annibale() ? adreno8_context_last() : legacy_gpu_last();
}

void
redwood_gpu_speed(uint32_t *mhz, uint32_t *changes)
{
	*mhz = *changes = 0;
	if (redwood_platform_is_annibale()) {
		adreno8_speed_state(mhz, changes);
	}
}

void
redwood_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	if (!redwood_platform_is_annibale()) {
		legacy_gpu_worked(microseconds, submissions, by);
		return;
	}
	adreno8_hfi_worked(microseconds, submissions, by);
}

void
redwood_gpu_report(const char *why)
{
	if (!redwood_platform_is_annibale()) {
		legacy_gpu_report(why);
		return;
	}
	/* Both callers report failed execution: a completion timeout or a zero submit fence. */
	adreno8_say("adreno8: RedwoodGPU %s; last timestamp %u; SMMU fault %u\n",
	    why, adreno8_context_last(), adreno8_gpu_faulted());
	adreno8_bo_stop();
}

bool
redwood_gpu_recover(void)
{
	return redwood_platform_is_annibale() ? false : legacy_gpu_recover();
}

void
redwood_gpu_stop(void)
{
	if (!redwood_platform_is_annibale()) {
		legacy_gpu_stop();
		return;
	}
	if (adreno8_bo_ready()) (void)adreno8_context_close();
	adreno8_bo_stop();
}
