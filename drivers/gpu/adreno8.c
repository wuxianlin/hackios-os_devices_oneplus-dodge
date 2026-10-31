/*
 * SM8750 / Adreno 830 initialization probes. No GPU access occurs without adreno8=N.
 * Each step checks its prerequisites and stops the sequence on failure. Register accesses
 * are logged before execution so the resident loader can retain the last attempted access.
 *
 * Steps:
 *   1  QFPROM speed_bin, CMD DB rail levels and BCM descriptions
 *   2  GPUCC configuration reads
 *   3  CX power enable, preserving its original GDSCR configuration
 *   4  CX retention, fuses, GBIF and RSCC reads
 *   5  CX, GMU and bus clocks; default GMU source is the 19.2 MHz crystal
 *   6  GMU reset, power, HFI, interrupt and counter reads
 *   7  Two-word DTCM write/read with original data restored; requires CM3 reset
 *   8  SMMU capability, stream and context reads
 *   9  Firmware reservations, HFI queues and 4 KiB SMMU mappings for stream 5/context 2
 *  10  gen80000 GMU firmware load and readback verification
 *  11  SCM CX register initialization and GMU start, using inherited host DDR votes
 *  12  HFI queue start and version validation
 *  13  Performance and bandwidth tables
 *  14  Prepare GPU buffers, enable CB0 and start HWSCHED, servicing GMU memory requests
 *  15  160 MHz/BW2 DCVS request and GMUCX status; requires keep and a host cold reset
 *  16  Read back GPU SMMU context 0, routing, page tables and fault status
 *  17  OOB, GPU registers, SQE/AQE and CP initialization
 *  18  PAS 13 zap authentication
 *  19  Leave secure mode
 *  20  GPU scratch write/read verification
 *  21  Fault, queue, power and counter diagnostics
 *  22  Standard HWSCHED context: two non-privileged IBs, timestamps and unregister
 *  23  Enable external BO mappings and publish the RedwoodGPU service
 *
 * adreno8=N           execute steps 1..N
 * adreno8_only=K      execute only step K; its prerequisites must already exist in this kernel
 * adreno8_skip=MASK   skip selected steps (bit 0 selects step 1)
 * adreno8_pause=MS    pause before access groups in the newest step (default 2200 ms)
 * adreno8_fine=1      also pause before individual accesses in that step
 * adreno8_raw_nop=N   stop before CP_INIT: 1=two RAW NOP words, 2=ten RAW NOP words,
 *                  3=setup-only health checks without RAW publication
 * adreno8_direct_ring=1  ordinary HFI and one direct ring; stage17/keep1/return0/gmuclk2,
 *                  secure_preflight0, no only/skip/empty_powerup; raw_nop2 writes back inherited SQE control 0,
 *                  raw_nop3 stops before SQE, default stops after CP_INIT idle
 * adreno8_empty_powerup=1  stage-17 CP_INIT with a zeroed powerup page; stop after idle;
 *                  requires keep=1 return=0, no only/skip/raw_nop; default uses the generated list
 * adreno8_watch=S     inspect GMU status for S seconds after step 11 or later (default 20)
 * adreno8_keep=1      retain resources; stage 15 requires a cold reset before memory reuse
 * adreno8_secure_preflight=N  stage-14 secure memory mode. Mode 0 and unknown values
 *                  stop after the read-only preflight; mode 1 reports the preflight result.
 *                  Mode 2 transfers one kind-5 buffer and its tables, then stops without a
 *                  reply. Mode 3 also configures context 3 while stream 2 remains faulted.
 *                  Mode 4 repeats that setup in the same boot, validates the translate
 *                  route, then replies to MEM_ALLOC. READY permits normal allocations and
 *                  exact replay of the original secure request; new secure handles fail.
 *                  All transferred pages remain allocated until cold reset.
 * adreno8_cbscan=1    diagnostic only: after the step-8 SMMU read, print context-bank 0-5 and
 *                  20 registers (SCTLR, TTBR0, TCR, FSR) and read context 3's SCTLR twice;
 *                  read-only, the values enter no decision, off unless asked
 * adreno8_gpu_stall=1  diagnostic only: stall faults in GPU contexts 0 and 3; poll fault
 *                  registers with context fault interrupts disabled; GMU context 2 is unchanged
 * adreno8_arc_read=1  stage-1 diagnostic: Apps DRV2 cx.lvl then mx.lvl reads; requires explicit
 *                  touchscreen=0 and a host cold reset after any register write
 * adreno8_speed=N     fastest GX speed: 1 (160 MHz) stays at 160 MHz; 2 (222 MHz) or 3 (342 MHz)
 *                  lets the speed follow load and touches from 160 MHz up to it (adreno8_speed_tick)
 * adreno8_inflight=N  submissions the context queue may hold ahead of the GPU, 1 to 8 (default 4);
 *                  1 waits for each submission before the next, as stage 23 first did
 * adreno8_gmuclk=1    retain CX/MX votes, initialize GMU 650/HUB 200 MHz, then GMU 500
 *                  after START; requires touchscreen=0 wfi=0 wfe_mode=0; default uses the crystal
 * adreno8_gmuclk=2    full stages 1..5 or 1..17 and later: highest supported CX/MX WRITE completion;
 *                  same explicit zero parameters, no preceding ARC READ, keep=1 return=0;
 *                  retain requests until cold reset; stage 17 and later permit START and CP commands
 * adreno8_return=1    return to the resident loader after cleanup
 *
 * Firmware family is gen80000, as loaded by Android and specified by the vendor gen8.0.0
 * configuration. Test images use the linux-firmware files under /fw. Addresses come from
 * the annibale DT; register definitions are checked against the SM8750 vendor driver and
 * Linux v6.19. GMU dword indices use the GPU base 0x3d00000; GMU(index) converts them to
 * offsets in the DT window at 0x3d37000.
 *
 * adreno8_cx.c owns clocks, power and SCM; adreno8_gmu.c owns firmware and GMU startup;
 * adreno8_smmu.c owns address translation; adreno8_hfi.c owns HFI. Shared declarations and
 * wire layouts are in the private headers. socinfo SKU selection, host DDR voting, GPU
 * MMU/OOB startup and CP probes are being verified. Stage 23 publishes a GPU service only
 * after the standard context self-test succeeds.
 */
#include "adreno8_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_gpu_private.h"
#include "adreno8_zap_private.h"

uint32_t adreno8_skip_steps, adreno8_pause_ms = 2200, adreno8_fine, adreno8_watch_seconds = 20;

uint32_t adreno8_speed_asked;                  /* adreno8_speed: the speed once drawing, 1 the slowest (adreno8_speed_tick) */

uint32_t adreno8_inflight = 4;                 /* adreno8_inflight (adreno8_context_submit) */

uint32_t adreno8_gmu_from_pll;                 /* adreno8_gmuclk */

bool adreno8_direct_ring;                      /* cold-boot ordinary HFI/CP_INIT diagnostic */
bool adreno8_gpu_stall;                        /* adreno8_gpu_stall=1 */

uint32_t adreno8_step;                         /* the one being performed */

uint32_t adreno8_newest_step;                  /* the highest one asked for: the one that has not been seen to work */

char adreno8_last_write[96] = "nothing written";

bool adreno8_quiet;                            /* repeating accesses that have been announced and seen to work: say nothing */

int
adreno8_say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define printf adreno8_say

void
adreno8_sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

void
adreno8_sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- register access ------------------------------------------------------------------------------- */

/*
 * Some Qualcomm hypervisors emulate SMMU global-register accesses and do not handle
 * XZR/WZR stores. Keep a simple general-register ldr/str form; emulation coverage
 * on this device is unconfirmed.
 */

uint32_t __attribute__((noinline))
adreno8_rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

extern void loader_log_line(const char *line);

void __attribute__((noinline))
adreno8_wr(vm_offset_t address, uint32_t value)
{
	/* CP_SQE_CNTL is dword 0x815; these RAM-only lines surround the raw store after any access pause. */
	bool ordinary_sqe = adreno8_direct_ring && adreno8_gpu.base && address == adreno8_gpu.base + 0x2054;
	if (ordinary_sqe) loader_log_line("adreno8: ordinary SQE raw STR enter\n");
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
	if (ordinary_sqe) loader_log_line("adreno8: ordinary SQE raw STR returned\n");
}

/* A stretch of registers; mapped when a step first needs it. */

void
adreno8_need(struct adreno8_block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */

void
adreno8_hold(bool single_access)
{
	if (adreno8_step == adreno8_newest_step && adreno8_pause_ms && (!single_access || adreno8_fine)) {
		adreno8_sleep_ms(adreno8_pause_ms);
	}
}

/* What a step is about to do, in a line. */

void
adreno8_announce(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("adreno8: step %u: %s\n", adreno8_step, line);
	adreno8_hold(false);
}

uint32_t
adreno8_get(struct adreno8_block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	adreno8_need(b);
	if (adreno8_quiet) {
		return adreno8_rd(b->base + offset);
	}
	if (adreno8_step == adreno8_newest_step) {
		printf("adreno8: %u: about to read %s %s at %#x\n", adreno8_step, b->name, name, b->phys + offset);
		adreno8_hold(true);
	}
	value = adreno8_rd(b->base + offset);
	printf("adreno8: %u: %s %s (%#x) = %#x\n", adreno8_step, b->name, name, b->phys + offset, value);
	return value;
}

void
adreno8_put(struct adreno8_block *b, const char *name, uint32_t offset, uint32_t value)
{
	adreno8_need(b);
	if (!adreno8_quiet) {
		printf("adreno8: %u: about to write %s %s at %#x: %#x\n", adreno8_step, b->name, name, b->phys + offset, value);
		snprintf(adreno8_last_write, sizeof(adreno8_last_write), "step %u wrote %#x to %s %s at %#x", adreno8_step, value, b->name, name,
		    b->phys + offset);
		adreno8_hold(true);
	}
	adreno8_wr(b->base + offset, value);
}

/* Set bits of a register unless they are set. Returns what the register holds afterwards. */

uint32_t
adreno8_set(struct adreno8_block *b, const char *name, uint32_t offset, uint32_t bits)
{
	uint32_t was = adreno8_get(b, name, offset);

	if ((was & bits) == bits) {
		return was;
	}
	adreno8_put(b, name, offset, was | bits);
	return adreno8_get(b, name, offset);
}

/* Look at a register that has been read before until the masked bits have the value, for so many milliseconds. */

bool
adreno8_wait_for(struct adreno8_block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((adreno8_rd(b->base + offset) & mask) == value) {
			return true;
		}
		adreno8_sleep_us(100);
	}
	return (adreno8_rd(b->base + offset) & mask) == value;
}

/* ---- leaving --------------------------------------------------------------------------------------------- */

bool adreno8_gpu8_keep, adreno8_gpu8_return;
uint32_t adreno8_gpu8_secure_mode;
bool adreno8_reset_required;

/* Stop GMU access before releasing mapped memory; restore CX configuration if changed. */
static void
adreno8_gpu8_block_stop(void)
{
	if (adreno8_reset_required || adreno8_memory_retained()) {
		printf("adreno8: stop: GPU shutdown requires host cold reset; retaining memory and mappings\n");
		return;
	}
	printf("adreno8: stop: stopping GMU and releasing driver resources\n");
	if (adreno8_gmu_runs || adreno8_rsc_woken_by_us) {
		adreno8_wr(adreno8_gmu.base + GMU_GMU2HOST_MASK, ~0u);     /* nothing it says can wake anyone after us */
		adreno8_wr(adreno8_gmu.base + GMU_AHB_FENCE_CLR, ~0u);
		if (adreno8_gmu_runs) {
			adreno8_wr(adreno8_gmu.base + GMU_CM3_SYSRESET, 1);
			printf("adreno8: stop: the GMU's processor back in reset; its firmware stops with its memory\n");
		}
		adreno8_gmu_runs = adreno8_gmu_talks = false;
		if (adreno8_rsc_woken_by_us) {
			adreno8_wr(adreno8_gmu.base + GMU_RSCC_CONTROL_REQ, adreno8_rd(adreno8_gmu.base + GMU_RSCC_CONTROL_REQ) & ~2u);
			printf("adreno8: stop: our RSC wake request dropped; it may sleep again\n");
			adreno8_rsc_woken_by_us = false;
		}
	}
	if (adreno8_gmu_space.is_on) {
		adreno8_quiet = true;                               /* the last few writes say nothing: stop is meant to be short */
		adreno8_space_off(&adreno8_gmu_space, GMU_MATCH);
		adreno8_quiet = false;
		if (adreno8_gmu_space.is_on) {
			printf("adreno8: stop: SMMU shutdown incomplete; retaining mapped memory and CX power\n");
			return;
		}
		printf("adreno8: stop: the GMU's translation context off again\n");
	}
	for (unsigned i = 0; i < adreno8_gmu_space.table_count; i++) {
		if (adreno8_gmu_space.tables[i].table != 0) {
			kmem_free(kernel_map, adreno8_gmu_space.tables[i].table, PAGE_SIZE);
		}
	}
	adreno8_gmu_space.table_count = 0;
	if (adreno8_cx_changed) {
		adreno8_put(&adreno8_gpucc, "cx gdscr: restore saved configuration", GPUCC_CX_GDSCR, adreno8_cx_saved_gdscr);
		adreno8_cx_changed = false;
	}
	for (unsigned i = 0; i < adreno8_gmu_demand_count; i++) {
		if (adreno8_gmu_demands[i].memory != 0) {
			kmem_free(kernel_map, adreno8_gmu_demands[i].memory, (adreno8_gmu_demands[i].size + PAGE_MASK) & ~PAGE_MASK);
			adreno8_gmu_demands[i].memory = 0;
		}
	}
	adreno8_gmu_demand_count = 0;
	for (unsigned i = 0; i < 2; i++) {
		struct adreno8_gpu_block *b = i == 0 ? &adreno8_gmu_log : &adreno8_gmu_hfi;

		if (b->memory != 0) {
			kmem_free(kernel_map, b->memory, (b->size + PAGE_MASK) & ~PAGE_MASK);
			b->memory = 0;
		}
	}
	adreno8_fw_loaded = false;
	memset(adreno8_log_view, 0, sizeof(adreno8_log_view));
	adreno8_hfi_view_table = adreno8_hfi_view_cmd = adreno8_hfi_view_resp = adreno8_hfi_view_debug = 0;
	/* ml_io_map_wcomb has no unmap API. Clear aliases before their backing memory is reused. */
	printf("adreno8: stop: mapped memory released; CX configuration restored if changed\n");
}

/* ---- the walk ----------------------------------------------------------------------------------------------- */

static bool
adreno8_step_core_start(void)
{
	if (!adreno8_gpu8_keep || adreno8_gpu8_return) {
		printf("adreno8: GPU startup probes require keep=1, return=0 and host cold reset\n");
		return false;
	}
	return adreno8_gpu_prepare() && adreno8_gpu_memory_enable() &&
	    (adreno8_direct_ring ? adreno8_ordinary_start() : adreno8_hwsched_start());
}

static bool
adreno8_step_zap(void)
{
	return adreno8_gpu_cp_ready() && adreno8_zap_authenticate();
}

static bool
adreno8_step_secure_exit(void)
{
	return adreno8_zap_ready() && adreno8_gpu_secure_exit();
}

/* Execute requested stages in order and stop at the first failed prerequisite. */
void
adreno8_init(void)
{
	static bool (*const each[])(void) = {
		adreno8_step_outside, adreno8_step_gpucc, adreno8_step_cx_power, adreno8_step_cx_awake, adreno8_step_clocks,
		adreno8_step_first_words, adreno8_step_gmu_memory, adreno8_step_smmu_read, adreno8_step_gmu_space, adreno8_step_firmware,
		adreno8_step_gmu_start, adreno8_step_queues, adreno8_step_tables, adreno8_step_core_start,
		adreno8_step_gx_power, adreno8_gpu_memory_verify, adreno8_gpu_cp_init, adreno8_step_zap,
		adreno8_step_secure_exit, adreno8_gpu_scratch_test, adreno8_step_last_look, adreno8_context_test,
		adreno8_step_service
	};
	const unsigned count = sizeof(each) / sizeof(each[0]);
	uint32_t asked = 0, only = 0, one = 0, completed = 0, empty_powerup = 0, raw_nop = 0;
	uint32_t direct_ring = 0, secure_mode_arg = 0;

	if (!PE_parse_boot_argn("adreno8", &asked, sizeof(asked)) || asked == 0) {
		return;
	}
	PE_parse_boot_argn("adreno8_only", &only, sizeof(only));
	PE_parse_boot_argn("adreno8_pause", &adreno8_pause_ms, sizeof(adreno8_pause_ms));
	PE_parse_boot_argn("adreno8_watch", &adreno8_watch_seconds, sizeof(adreno8_watch_seconds));
	PE_parse_boot_argn("adreno8_speed", &adreno8_speed_asked, sizeof(adreno8_speed_asked));
	PE_parse_boot_argn("adreno8_inflight", &adreno8_inflight, sizeof(adreno8_inflight));
	PE_parse_boot_argn("adreno8_gmuclk", &adreno8_gmu_from_pll, sizeof(adreno8_gmu_from_pll));
	adreno8_gpu8_keep = PE_parse_boot_argn("adreno8_keep", &one, sizeof(one)) && one != 0;
	one = 0;
	adreno8_gpu8_return = PE_parse_boot_argn("adreno8_return", &one, sizeof(one)) && one != 0;
	one = 0;
	if (PE_parse_boot_argn("adreno8_secure_preflight", &one, sizeof(one))) {
		if (one <= SECURE_MODE_TRANSLATION_B) {
			adreno8_gpu8_secure_mode = one;
		} else {
			printf("adreno8: adreno8_secure_preflight=%u is not a known mode; the secure path stays off\n", one);
		}
	}
	secure_mode_arg = one;
	one = 0;
	if (PE_parse_boot_argn("adreno8_cbscan", &one, sizeof(one)) && one != 0) {
		adreno8_smmu_cb_scan_ask();
	}
	one = 0;
	adreno8_fine = PE_parse_boot_argn("adreno8_fine", &one, sizeof(one)) && one != 0;
	one = 0;
	adreno8_gpu_stall = PE_parse_boot_argn("adreno8_gpu_stall", &one, sizeof(one)) && one == 1;
	PE_parse_boot_argn("adreno8_skip", &adreno8_skip_steps, sizeof(adreno8_skip_steps));
	PE_parse_boot_argn("adreno8_empty_powerup", &empty_powerup, sizeof(empty_powerup));
	PE_parse_boot_argn("adreno8_raw_nop", &raw_nop, sizeof(raw_nop));
	PE_parse_boot_argn("adreno8_direct_ring", &direct_ring, sizeof(direct_ring));
	adreno8_direct_ring = direct_ring == 1;
	if (direct_ring && (direct_ring != 1 || asked != 17 || only != 0 ||
	    adreno8_skip_steps != 0 || !adreno8_gpu8_keep || adreno8_gpu8_return || (raw_nop != 0 && raw_nop != 2 && raw_nop != 3) ||
	    empty_powerup != 0 || secure_mode_arg != 0 || adreno8_gmu_from_pll != 2)) {
		printf("adreno8: direct_ring=1 requires complete adreno8=17, keep=1 return=0 gmuclk=2 secure_preflight=0 raw_nop=0/2/3 without only, skip or empty_powerup; no steps attempted\n");
		return;
	}
	if (empty_powerup && (empty_powerup != 1 || asked != 17 || only != 0 ||
	    adreno8_skip_steps != 0 || !adreno8_gpu8_keep || adreno8_gpu8_return || raw_nop != 0)) {
		printf("adreno8: empty_powerup=1 requires adreno8=17, keep=1 return=0 without only, skip or raw_nop; no steps attempted\n");
		return;
	}
	if (adreno8_gmu_from_pll == 2 && ((asked != 5 && asked < 17) || only != 0 ||
	    adreno8_skip_steps != 0 || !adreno8_gpu8_keep || adreno8_gpu8_return)) {
		printf("adreno8: gmuclk=2 requires adreno8=5 or 17 and later, keep=1 return=0 without only or skip; no steps attempted\n");
		return;
	}
	if (asked > count) {
		asked = count;
	}
	/* Announce and pause before accesses in the requested step, including its first read. */
	adreno8_newest_step = only != 0 ? only : asked;
	printf("adreno8: the Adreno 830 of SM8750, step by step: up to step %u of %u asked for%s%s (only, skip, pause, fine, watch, keep, speed, gmuclk, return all hang off the same name)\n",
	    asked, count, only != 0 ? " -- but only one step will run" : "", adreno8_gpu8_keep ? " -- and everything stays up" : "");
	for (unsigned i = 0; i < count; i++) {
		unsigned number = i + 1;

		if (only != 0 ? number != only : number > asked || (adreno8_skip_steps >> i & 1) != 0) {
			continue;
		}
		adreno8_step = number;
		if (!each[i]()) {
			printf("adreno8: %u: FAILED; the steps after it are not attempted\n", number);
			break;
		}
		completed = number;
		printf("adreno8: %u: completed\n", number);
		adreno8_hold(false);
	}
	if (adreno8_watch_seconds != 0 && completed >= 11 && adreno8_gmu.base != 0) {
		for (unsigned s = 1; s <= adreno8_watch_seconds; s++) {
			adreno8_sleep_ms(1000);
			printf("adreno8: alive %u s after step %u: the GMU's counter %#x%08x, its log %u lines, its interrupts %#x, the power word %#x\n",
			    s, completed, adreno8_rd(adreno8_gmu.base + GMU_ALWAYS_ON_HI), adreno8_rd(adreno8_gmu.base + GMU_ALWAYS_ON_LO),
			    adreno8_rd(adreno8_gmu.base + GMU_GENERAL_9) & 0xffff, adreno8_rd(adreno8_gmu.base + GMU_AO_INTERRUPT_STATUS),
			    adreno8_rd(adreno8_gmu.base + GMU_PWR_CLK_STATUS));
		}
	}
	if (adreno8_gpu8_keep) {
		printf("adreno8: keeping everything up (adreno8_keep): the GMU's firmware goes on running and answering\n");
	} else if (completed >= 3 || adreno8_cx_changed) {
		adreno8_gpu8_block_stop();
	}
	printf("adreno8: over; the system goes on starting\n");
	if (adreno8_gpu8_return) {
		printf("adreno8: adreno8_return: the boot goes on from in here, as redwood's adreno_return does\n");
		loader_return();
	}
}

/*
 * The boot is about to hand memory out again (loader_return): make sure nothing of the GPU's can
 * still write into it or wake anyone afterwards. Cheap and guarded: on a walk that went well the
 * stop already did all of this.
 */
void
adreno8_quiesce(void)
{
	/* CM3 reset and SMMU disable do not remove GMU RPMh votes. Until RSC shutdown
	 * is verified, preserve all resources and prevent the loader from reusing memory. */
	if (adreno8_reset_required || adreno8_memory_retained()) {
		const char *message = "\nadreno8: GPU probe requires host cold reset before reusing kernel memory\n";
		while (*message) {
			loader_log_putc(*message++);
		}
		for (;;) {
			__asm__ volatile ("wfe");
		}
	}
	if (adreno8_gmu.base != 0 && adreno8_gmu_runs) {
		adreno8_wr(adreno8_gmu.base + GMU_GMU2HOST_MASK, ~0u);
		adreno8_wr(adreno8_gmu.base + GMU_AHB_FENCE_CLR, ~0u);
		adreno8_wr(adreno8_gmu.base + GMU_CM3_SYSRESET, 1);
		adreno8_gmu_runs = false;
	}
	if (adreno8_gmu_space.is_on) {
		adreno8_quiet = true;
		adreno8_space_off(&adreno8_gmu_space, GMU_MATCH);
		adreno8_quiet = false;
		if (adreno8_gmu_space.is_on) {
			const char *message = "\nadreno8: SMMU shutdown incomplete; host reset required before reusing kernel memory\n";
			while (*message) {
				loader_log_putc(*message++);
			}
			/* The resident loader still accepts USB reboot; its memory must not be reused yet. */
			for (;;) {
				__asm__ volatile ("wfe");
			}
		}
	}
}
