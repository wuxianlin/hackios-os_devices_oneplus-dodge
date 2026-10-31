/* SM8750 GMU memory, firmware loading, startup and diagnostics. */
#include "adreno8_memory_private.h"

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#define printf adreno8_say

/* ---- the GMU's own window ------------------------------------------------------------------------------- */

/*
 * dt: qcom,gmu@3d37000 (reg 0x3d37000 length 0x68000). The xml's numbers are dword indices in the
 * GPU block's space based at 0x3d00000, so the window's byte offset of dword n is 4n - 0x37000 --
 * Linux's GMU_BYTE_OFFSET with the vendor's own window base (Linux maps 0x3d2c000 and subtracts
 * 0x2c000; the phone's device tree says the firmware driver gets 0x3d37000 and 0x37000).
 */

struct adreno8_block adreno8_gmu = { "gmu", 0x03d37000, 0x68000 };

/* sun-gpu.dtsi at d50b305f7da9: kgsl_3d0_reg_memory, ending before gmu_ao_blk_dec0. */
struct adreno8_block adreno8_gpu = { "gpu", 0x03d00000, 0x40000 };

/* where the memory the firmware asks for lives: the addresses are the firmware's, in its space */

struct adreno8_gpu_block adreno8_gmu_demands[GMU_BLOCKS];

unsigned adreno8_gmu_demand_count;

struct adreno8_gpu_block adreno8_gmu_log, adreno8_gmu_hfi;

bool adreno8_gmu_runs, adreno8_gmu_talks, adreno8_fw_loaded, adreno8_rsc_woken_by_us;

vm_offset_t adreno8_log_view[4];               /* the GMU's log, uncached: it writes while our caches guess */

vm_offset_t adreno8_hfi_view_table, adreno8_hfi_view_cmd, adreno8_hfi_view_resp, adreno8_hfi_view_debug;
static const struct adreno8_memory *adreno8_gmu_vrb;

bool
adreno8_block_new(struct adreno8_gpu_block *b, const char *name, uint32_t address, uint32_t size)
{
	const struct adreno8_gpu_block *fixed[2] = { &adreno8_gmu_log, &adreno8_gmu_hfi };
	uint32_t rounded;
	kern_return_t allocation;

	if (b->memory || size == 0 || size > GMU_BLOCK_CAP || (address & 0xfff) ||
	    adreno8_fw_region(address, size) != ADRENO8_FW_MEMORY) {
		printf("adreno8: %u: invalid allocation %s at %#x, size %#x\n", adreno8_step, name, address, size);
		return false;
	}
	/* GMU translations use 4 KiB granules even if XNU allocates larger pages. */
	rounded = (size + 0xfff) & ~0xfffu;
	if (adreno8_fw_region(address, rounded) != ADRENO8_FW_MEMORY) {
		return false;
	}
	for (unsigned i = 0; i < adreno8_gmu_demand_count + 2; i++) {
		const struct adreno8_gpu_block *other = i < adreno8_gmu_demand_count ? &adreno8_gmu_demands[i] : fixed[i - adreno8_gmu_demand_count];

		if (other->memory && adreno8_ranges_overlap(address, rounded, other->address, (other->size + 0xfff) & ~0xfffu)) {
			printf("adreno8: %u: %s overlaps %s after page alignment\n", adreno8_step, name, other->name);
			return false;
		}
	}
	/* HFI queues cross allocation pages within one uncached physical mapping. */
	if (b == &adreno8_gmu_hfi) {
		allocation = kmem_alloc_contig(kernel_map, &b->memory, (vm_size_t)((size + PAGE_MASK) & ~PAGE_MASK),
		    PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	} else {
		allocation = kmem_alloc(kernel_map, &b->memory, (vm_size_t)((size + PAGE_MASK) & ~PAGE_MASK),
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	}
	if (allocation != KERN_SUCCESS) {
		printf("adreno8: %u: no kernel memory for the GMU's %#x bytes\n", adreno8_step, size);
		b->memory = 0;
		return false;
	}
	b->name = name;
	b->address = address;
	b->size = size;
	adreno8_to_memory(b->memory, rounded);
	printf("adreno8: %u: memory for the GMU's %#x bytes at its %#x (%s): kernel %#llx; pages at %#llx", adreno8_step, size,
	    address, name, (uint64_t)b->memory, kvtophys(b->memory));
	printf(" .. %#llx (%u allocation pages)\n", kvtophys(b->memory + ((size - 1) & ~PAGE_MASK)),
	    (unsigned)((size + PAGE_MASK) / PAGE_SIZE));
	return true;
}

/* The writes of this processor have reached memory before the GMU reads it (redwood: settle). */

void
adreno8_settle(void)
{
	/* dsb sy orders every earlier access of this PE, at all shareability
	 * levels, before every later one. The memory clobber keeps the
	 * compiler from moving C-level loads and stores across the barrier;
	 * the asm alone fixes only the barrier's own position. */
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Push what this processor has written to a stretch of memory out of its cache, and forget it: the GMU reads memory. */

void
adreno8_to_memory(vm_offset_t start, vm_size_t size)
{
	/* The order the GMU or the secure world needs to read a buffer this
	 * kernel just built: the CPU stores the content, dc civac pushes each
	 * dirty 64-byte line out to its point of coherence, and the trailing
	 * dsb sy waits for those cache maintenance operations to complete;
	 * only then may the SMC call tell the other side to read. The memory
	 * clobber on each line constrains the compiler: a store of buffer
	 * content stays above the line that cleans its address instead of
	 * being sunk into or past the loop. The host test mocks run on
	 * ordinary memory and prove nothing about this order. */
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	adreno8_settle();
}

/* Whether the memory for this address was already made (a demand of the file, or one of ours). */

bool
adreno8_covered(uint32_t address, uint32_t size)
{
	const struct adreno8_gpu_block *fixed[2] = { &adreno8_gmu_log, &adreno8_gmu_hfi };

	for (unsigned i = 0; i < adreno8_gmu_demand_count; i++) {
		if (adreno8_gmu_demands[i].memory && adreno8_range_contains(adreno8_gmu_demands[i].address, adreno8_gmu_demands[i].size, address, size)) {
			return true;
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		if (fixed[i]->memory && adreno8_range_contains(fixed[i]->address, fixed[i]->size, address, size)) {
			return true;
		}
	}
	return false;
}

/* The kernel address of the GMU's address, or nothing. */

vm_offset_t
adreno8_seen_at(uint32_t address)
{
	const struct adreno8_gpu_block *fixed[2] = { &adreno8_gmu_log, &adreno8_gmu_hfi };

	for (unsigned i = 0; i < adreno8_gmu_demand_count; i++) {
		if (adreno8_gmu_demands[i].memory && adreno8_range_contains(adreno8_gmu_demands[i].address, adreno8_gmu_demands[i].size, address, 1)) {
			return adreno8_gmu_demands[i].memory + (address - adreno8_gmu_demands[i].address);
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		if (fixed[i]->memory && adreno8_range_contains(fixed[i]->address, fixed[i]->size, address, 1)) {
			return fixed[i]->memory + (address - fixed[i]->address);
		}
	}
	return 0;
}

/* What the GMU and its firmware say about themselves, printed when something of theirs failed. */

void
adreno8_gmu_post_mortem(void)
{
	uint32_t wptr, entries;

	if (adreno8_gmu.base == 0) {
		return;
	}
	printf("adreno8: %u: CM3 reset %#x, boot config %#x, config %#x, GMU clock branch %#x, source %#x\n",
	    adreno8_step, adreno8_rd(adreno8_gmu.base + GMU_CM3_SYSRESET), adreno8_rd(adreno8_gmu.base + GMU_CM3_BOOT_CONFIG),
	    adreno8_rd(adreno8_gmu.base + GMU_CM3_CFG), adreno8_rd(adreno8_gpucc.base + GPUCC_CX_GMU_CBCR),
	    adreno8_rd(adreno8_gpucc.base + GPUCC_GMU_RCGR + 4));
	if (adreno8_gmu_space.is_on) {
		printf("adreno8: %u: SMMU fault check: %s\n", adreno8_step,
		    adreno8_space_faulted(&adreno8_gmu_space) ? "fault present" : "no fault bits set");
	}
	printf("adreno8: %u: what the GMU says now: init result %#x, hfi %#x, gmu2host info %#x mask %#x, ao interrupts %#x, fence %#x, power/clk %#x\n",
	    adreno8_step, adreno8_rd(adreno8_gmu.base + GMU_FW_INIT_RESULT), adreno8_rd(adreno8_gmu.base + GMU_HFI_CTRL_STATUS), adreno8_rd(adreno8_gmu.base + GMU_GMU2HOST_INFO),
	    adreno8_rd(adreno8_gmu.base + GMU_GMU2HOST_MASK), adreno8_rd(adreno8_gmu.base + GMU_AO_INTERRUPT_STATUS), adreno8_rd(adreno8_gmu.base + GMU_AHB_FENCE_STATUS),
	    adreno8_rd(adreno8_gmu.base + GMU_PWR_CLK_STATUS));
	printf("adreno8: %u: RSC status %#x, sequence busy %#x\n", adreno8_step,
	    adreno8_rd(adreno8_rscc.base + RSCC_RSC_STATUS), adreno8_rd(adreno8_rscc.base + RSCC_SEQ_BUSY));
	for (unsigned q = 0; adreno8_hfi_view_table != 0 && q < ADRENO8_HFI_QUEUE_COUNT; q++) {
		vm_offset_t h = adreno8_hfi_view_table + ADRENO8_HFI_HEADER(q) * 4;
		printf("adreno8: %u: HFI queue %u status %u, read %u, write %u\n", adreno8_step, q,
		    adreno8_rd(h), adreno8_rd(h + 40), adreno8_rd(h + 44));
	}
	if (adreno8_log_view[0] == 0 || adreno8_gmu.base == 0) {
		return;
	}
	wptr = adreno8_rd(adreno8_gmu.base + GMU_GENERAL_9) & 0xffff;
	entries = wptr < 24 ? wptr : 24;
	printf("adreno8: %u: the GMU's log, last %u lines of %u it wrote:\n", adreno8_step, entries, wptr);
	for (uint32_t i = wptr - entries; i < wptr; i++) {
		uint32_t off = (i * 16) % 0x4000, page = off / 4096;

		if (adreno8_log_view[page] == 0) {
			continue;
		}
		printf("adreno8: %u:   log %u: %08x %08x %08x %08x\n", adreno8_step, i, adreno8_rd(adreno8_log_view[page] + off % 4096),
		    adreno8_rd(adreno8_log_view[page] + off % 4096 + 4), adreno8_rd(adreno8_log_view[page] + off % 4096 + 8), adreno8_rd(adreno8_log_view[page] + off % 4096 + 12));
	}
}

/* ---- step 6: the first words from the GMU ---------------------------------------------------------------- */

bool
adreno8_step_first_words(void)
{
	uint32_t power;

	if (!adreno8_cx_ready()) {
		return false;
	}
	adreno8_announce("read the GMU's whole front room at once, touching nothing: reset and boot words, its firmware's result word and the HFI queue state, the power and RPMh state, the interrupts old and new, the fence, its counters, and the three words the firmware is started with");
	adreno8_get(&adreno8_gmu, "cm3 sysreset", GMU_CM3_SYSRESET);
	adreno8_get(&adreno8_gmu, "cm3 boot config", GMU_CM3_BOOT_CONFIG);
	adreno8_get(&adreno8_gmu, "cm3 config", GMU_CM3_CFG);
	adreno8_get(&adreno8_gmu, "fw init result", GMU_FW_INIT_RESULT);
	adreno8_get(&adreno8_gmu, "hfi ctrl status", GMU_HFI_CTRL_STATUS);
	adreno8_get(&adreno8_gmu, "hfi qtbl info", GMU_HFI_QTBL_INFO);
	adreno8_get(&adreno8_gmu, "hfi qtbl address", GMU_HFI_QTBL_ADDR);
	power = adreno8_get(&adreno8_gmu, "power clock status", GMU_PWR_CLK_STATUS);
	if (power == 0xffffffff) {
		printf("adreno8: %u: the GMU does not answer at all: no clock, no power, or the hypervisor says no\n", adreno8_step);
		return false;
	}
	printf("adreno8: %u: the 3D core's power as the GMU sees it: %s\n", adreno8_step, GMU_PWR_GX_UP(power) ? "up" : "off");
	adreno8_get(&adreno8_gmu, "rpmh power state", GMU_RPMH_POWER_STATE);
	adreno8_get(&adreno8_gmu, "fal intf", GMU_CX_FAL_INTF);
	adreno8_get(&adreno8_gmu, "falnext intf", GMU_CX_FALNEXT_INTF);
	adreno8_get(&adreno8_gmu, "gmu2host set", GMU_GMU2HOST_SET);
	adreno8_get(&adreno8_gmu, "gmu2host clr", GMU_GMU2HOST_CLR);
	adreno8_get(&adreno8_gmu, "gmu2host info", GMU_GMU2HOST_INFO);
	adreno8_get(&adreno8_gmu, "gmu2host mask", GMU_GMU2HOST_MASK);
	adreno8_get(&adreno8_gmu, "host2gmu set", GMU_HOST2GMU_SET);
	adreno8_get(&adreno8_gmu, "ao interrupt status", GMU_AO_INTERRUPT_STATUS);
	adreno8_get(&adreno8_gmu, "ao interrupt mask", GMU_AO_INTERRUPT_MASK);
	adreno8_get(&adreno8_gmu, "rscc control req", GMU_RSCC_CONTROL_REQ);
	adreno8_get(&adreno8_gmu, "rscc control ack", GMU_RSCC_CONTROL_ACK);
	adreno8_get(&adreno8_gmu, "ahb fence ctrl", GMU_AHB_FENCE_CTRL);
	adreno8_get(&adreno8_gmu, "ahb fence range 0", GMU_AHB_FENCE_RANGE_0);
	adreno8_get(&adreno8_gmu, "ahb fence status", GMU_AHB_FENCE_STATUS);
	adreno8_get(&adreno8_gmu, "rbbm int unmasked", GMU_RBBM_INT_UNMASKED);
	adreno8_get(&adreno8_gmu, "general 8 (the log)", GMU_GENERAL_8);
	adreno8_get(&adreno8_gmu, "general 9 (log written)", GMU_GENERAL_9);
	adreno8_get(&adreno8_gmu, "general 10 (the chip)", GMU_GENERAL_10);
	adreno8_get(&adreno8_gmu, "always on counter, high", GMU_ALWAYS_ON_HI);
	adreno8_get(&adreno8_gmu, "always on counter, low", GMU_ALWAYS_ON_LO);
	if (adreno8_rd(adreno8_gmu.base + GMU_FW_INIT_RESULT) == GMU_FW_ALREADY) {
		printf("adreno8: %u: NOTE: the GMU's firmware is running already (its result word says so); steps 7 and 10 will refuse, and only a restart of the phone clears it\n", adreno8_step);
	}
	return true;
}

/* ---- step 7: its memory, in reset ------------------------------------------------------------------------ */

/*
 * Two words into the GMU's data memory and back, while its processor is in reset -- redwood's
 * step 10 unchanged but for the window. The word the firmware leaves at the end of that memory
 * says whether firmware is running, and a running firmware's memory is not ours to scribble on.
 */

bool
adreno8_step_gmu_memory(void)
{
	static const uint32_t first = 0xfeedfeee, second = 0xace5ba11;
	uint32_t save[2], result, readback[2];

	if (!adreno8_cx_ready()) {
		return false;
	}
	result = adreno8_get(&adreno8_gmu, "fw init result", GMU_FW_INIT_RESULT);
	if (result == GMU_FW_ALREADY || !(adreno8_get(&adreno8_gmu, "cm3 sysreset", GMU_CM3_SYSRESET) & 1)) {
		printf("adreno8: %u: CM3 reset is not confirmed; refusing the DTCM probe\n", adreno8_step);
		return false;
	}
	adreno8_announce("write two words into the GMU's data memory (0x3d71000) and read them back: the way its firmware goes in");
	for (unsigned i = 0; i < 2; i++) {
		save[i] = adreno8_rd(adreno8_gmu.base + GMU_DTCM + 4 * i);
	}
	printf("adreno8: %u: the GMU's data memory begins %#x %#x\n", adreno8_step, save[0], save[1]);
	adreno8_wr(adreno8_gmu.base + GMU_DTCM, first);
	adreno8_wr(adreno8_gmu.base + GMU_DTCM + 4, second);
	readback[0] = adreno8_rd(adreno8_gmu.base + GMU_DTCM);
	readback[1] = adreno8_rd(adreno8_gmu.base + GMU_DTCM + 4);
	/* Restore both words before reporting a failed probe. */
	adreno8_wr(adreno8_gmu.base + GMU_DTCM, save[0]);
	adreno8_wr(adreno8_gmu.base + GMU_DTCM + 4, save[1]);
	if (readback[0] != first || readback[1] != second) {
		printf("adreno8: %u: DTCM readback mismatch: %#x %#x; original words restored\n", adreno8_step, readback[0], readback[1]);
		return false;
	}
	printf("adreno8: %u: DTCM write/read passed; original words restored\n", adreno8_step);
	return true;
}

/* ---- step 9: memory for the GMU, and its translation ------------------------------------------------------ */

static bool adreno8_image_asked_quietly;            /* adreno8_image_file() says nothing: is the file there at all */

static const uint8_t *
adreno8_image_file(const char *directory, const char *name, uint64_t *size)
{
	int disk = -1;
	const uint8_t *data = hakios_image_file(directory, name, size, &disk);  /* (redwood_image.c) */

	if (data != NULL) {
		if (!adreno8_image_asked_quietly) {
			printf("adreno8: %u: %s/%s: %llu bytes in the image of /dev/md%d\n", adreno8_step, directory, name, *size, disk);
		}
		return data;
	}
	if (!adreno8_image_asked_quietly) {
		printf("adreno8: %u: no %s/%s in the small image: put matching /vendor/firmware/%s into fw/ and run "
		    "userland/build.sh\n", adreno8_step, directory, name, name);
	}
	return NULL;
}

static const uint8_t *adreno8_gmu_firmware;

static uint32_t adreno8_gmu_firmware_size;

static bool
adreno8_read_firmware(void)
{
	uint64_t bytes = 0;
	const uint8_t *data;

	if (adreno8_gmu_firmware) {
		return true;
	}
	data = adreno8_image_file("fw", "gen80000_gmu.bin", &bytes);
	if (!data || bytes > UINT32_MAX || !adreno8_fw_validate(data, (uint32_t)bytes)) {
		printf("adreno8: %u: missing or invalid gen80000 GMU firmware\n", adreno8_step);
		return false;
	}
	adreno8_gmu_firmware = data;
	adreno8_gmu_firmware_size = (uint32_t)bytes;
	return true;
}

/* Driver-owned regions follow the firmware's two uncached reservations. */

static const struct {
	const char *name;
	uint32_t address, size;
} adreno8_gmu_fixed[] = {
	{ "log",   0x60002000, 0x4000 },        /* where its firmware writes what it has to say (GENERAL_8) */
	{ "hfi",   0x60006000, ADRENO8_HFI_ALLOC_BYTES },
};

/* gen8_hwsched_gmu_memory_init publishes both buffers before CM3 starts.
 * The shared allocator owns their pages and mappings until cold reset. */
static bool
adreno8_gmu_hwsched_memory(void)
{
	const uint32_t prot = ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV;
	struct adreno8_memory *vrb = adreno8_gmu_alloc("gmu vrb", 4096, 4096, prot);
	struct adreno8_memory *trace;
	uint32_t *bank, *header;

	if (!vrb || !(trace = adreno8_gmu_alloc("gmu trace", 16384, 4096, prot)))
		return false;
	bank = (uint32_t *)vrb->memory;
	header = (uint32_t *)trace->memory;
	bank[0] = vrb->size / 4;
	bank[2] = trace->gmu_address;
	/* gmu_core_trace_header_init host inputs; other fields retain their zero allocation values. */
	header[1] = 0x40000001; /* drop mode, header version 1 */
	header[2] = 80; /* buffer threshold in percent */
	header[3] = trace->size;
	header[8] = trace->gmu_address; /* low dword of the 64-bit cookie */
	header[10] = 5; /* trace timeout in milliseconds */
	header[11] = 1; /* HWSCHED log type */
	if (!adreno8_memory_publish(trace, 0, 48) || !adreno8_memory_publish(vrb, 0, vrb->size))
		return false;
	adreno8_gmu_vrb = vrb;
	printf("adreno8: GMU VRB %#x size %#x, trace %#x size %#x published before CM3 start\n",
	    vrb->gmu_address, vrb->size, trace->gmu_address, trace->size);
	return true;
}

/* Types 1 and 7 reserve value bytes; other zero-size records contain metadata. */

bool
adreno8_step_gmu_space(void)
{
	uint32_t at = 0, wanted = 0;
	struct adreno8_fw_block block;

	if (!adreno8_cx_ready()) {
		return false;
	}
	if (!adreno8_read_firmware()) {
		return false;
	}
	adreno8_announce("walk the GMU's firmware file for the memory it asks for, and make it");
	while (at < adreno8_gmu_firmware_size) {
		if (!adreno8_fw_next(adreno8_gmu_firmware, adreno8_gmu_firmware_size, &at, &block)) {
			printf("adreno8: %u: malformed firmware record at %#x\n", adreno8_step, at);
			return false;
		}
		if (block.size != 0 || (block.type != 1 && block.type != 7) ||
		    adreno8_fw_region(block.address, block.value) != ADRENO8_FW_MEMORY) {
			continue;
		}
		if (adreno8_covered(block.address, block.value)) {
			continue;
		}
		if (adreno8_gmu_demand_count == GMU_BLOCKS) {
			printf("adreno8: %u: too many firmware reservations\n", adreno8_step);
			return false;
		}
		if (!adreno8_block_new(&adreno8_gmu_demands[adreno8_gmu_demand_count], "firmware reservation", block.address, block.value)) {
			return false;
		}
		adreno8_gmu_demand_count++;
		wanted += block.value;
	}
	printf("adreno8: %u: %u firmware reservations, %#x bytes\n", adreno8_step, adreno8_gmu_demand_count, wanted);

	adreno8_announce("allocate the GMU log and HFI queues");
	if (!adreno8_block_new(&adreno8_gmu_log, "gmu log", adreno8_gmu_fixed[0].address, adreno8_gmu_fixed[0].size) ||
	    !adreno8_block_new(&adreno8_gmu_hfi, "gmu hfi", adreno8_gmu_fixed[1].address, adreno8_gmu_fixed[1].size)) {
		return false;
	}
	adreno8_hfi_init_table((uint32_t *)adreno8_gmu_hfi.memory, adreno8_gmu_hfi.address, adreno8_direct_ring);
	adreno8_to_memory(adreno8_gmu_hfi.memory, ADRENO8_HFI_TABLE_DWORDS * 4u);

	adreno8_announce("map uncached CPU views of the HFI buffer and GMU log");
	adreno8_hfi_view_table = ml_io_map_wcomb(kvtophys(adreno8_gmu_hfi.memory), adreno8_gmu_hfi.size);
	if (!adreno8_hfi_view_table) {
		return false;
	}
	adreno8_hfi_view_cmd = adreno8_hfi_view_table + adreno8_hfi_queue_offset(0, adreno8_direct_ring);
	adreno8_hfi_view_resp = adreno8_hfi_view_table + adreno8_hfi_queue_offset(1, adreno8_direct_ring);
	adreno8_hfi_view_debug = adreno8_hfi_view_table + adreno8_hfi_queue_offset(2, adreno8_direct_ring);
	for (unsigned i = 0; i < 4; i++) {
		adreno8_log_view[i] = ml_io_map_wcomb(kvtophys(adreno8_gmu_log.memory + i * 4096), 4096);
		if (!adreno8_log_view[i]) {
			return false;
		}
	}

	adreno8_announce("map every block into the GMU's context at the address it asked for, and turn that context on");
	if (adreno8_space_new_table(&adreno8_gmu_space, 0, 0) == 0) {
		return false;
	}
	if (!adreno8_space_map_block(&adreno8_gmu_space, &adreno8_gmu_log) || !adreno8_space_map_block(&adreno8_gmu_space, &adreno8_gmu_hfi)) {
		return false;
	}
	for (unsigned i = 0; i < adreno8_gmu_demand_count; i++) {
		if (!adreno8_space_map_block(&adreno8_gmu_space, &adreno8_gmu_demands[i])) {
			return false;
		}
	}
	if (!adreno8_direct_ring && !adreno8_gmu_hwsched_memory()) {
		return false;
	}
	if (!adreno8_space_on(&adreno8_gmu_space, GMU_STREAM, GMU_MATCH)) {
		return false;
	}
	adreno8_smmu_cb_scan_after_step9();
	return true;
}

/* ---- step 10: the firmware itself ------------------------------------------------------------------------- */

/* Validate full ranges before writing; compare each segment after cache maintenance. */

bool
adreno8_step_firmware(void)
{
	uint32_t at = 0, written = 0, result;
	struct adreno8_fw_block block;

	adreno8_fw_loaded = false;
	if (!adreno8_cx_ready() || !adreno8_read_firmware()) {
		return false;
	}
	result = adreno8_rd(adreno8_gmu.base + GMU_FW_INIT_RESULT);
	if (result == GMU_FW_ALREADY || (result & GMU_FW_STARTED_MASK) == GMU_FW_STARTED) {
		printf("adreno8: %u: GMU firmware is already running; refusing TCM writes\n", adreno8_step);
		return false;
	}
	if (!adreno8_gmu_space.is_on) {
		printf("adreno8: %u: GMU translation must be enabled by step 9\n", adreno8_step);
		return false;
	}
	/* Recheck backing memory before the first write, including each segment's end. */
	while (at < adreno8_gmu_firmware_size) {
		if (!adreno8_fw_next(adreno8_gmu_firmware, adreno8_gmu_firmware_size, &at, &block)) {
			return false;
		}
		if (block.size && adreno8_fw_region(block.address, block.size) == ADRENO8_FW_MEMORY &&
		    !adreno8_covered(block.address, block.size)) {
			printf("adreno8: %u: no backing memory for firmware segment %#x + %#x\n", adreno8_step, block.address, block.size);
			return false;
		}
	}
	adreno8_put(&adreno8_gmu, "hold CM3 in reset for firmware loading", GMU_CM3_SYSRESET, 1);
	if ((adreno8_rd(adreno8_gmu.base + GMU_CM3_SYSRESET) & 1) == 0) {
		printf("adreno8: %u: CM3 reset did not assert\n", adreno8_step);
		return false;
	}
	adreno8_announce("load GMU firmware and verify each segment");
	at = 0;
	while (at < adreno8_gmu_firmware_size) {
		const uint8_t *payload;
		enum adreno8_fw_region region;
		uint32_t wrong = 0;

		if (!adreno8_fw_next(adreno8_gmu_firmware, adreno8_gmu_firmware_size, &at, &block)) {
			return false;
		}
		if (!block.size) {
			continue;
		}
		payload = adreno8_gmu_firmware + at - block.size;
		region = adreno8_fw_region(block.address, block.size);
		if (region == ADRENO8_FW_ITCM || region == ADRENO8_FW_DTCM) {
			uint32_t offset = region == ADRENO8_FW_ITCM ? GMU_ITCM + block.address : GMU_DTCM + block.address - GMU_DTCM_ADDRESS;

			for (uint32_t i = 0; i < block.size; i += 4) {
				uint32_t word;

				memcpy(&word, payload + i, sizeof(word));
				adreno8_wr(adreno8_gmu.base + offset + i, word);
			}
			adreno8_settle();
			for (uint32_t i = 0; i < block.size; i += 4) {
				uint32_t word;

				memcpy(&word, payload + i, sizeof(word));
				if (adreno8_rd(adreno8_gmu.base + offset + i) != word) {
					wrong++;
				}
			}
		} else {
			vm_offset_t where = adreno8_seen_at(block.address);

			memcpy((void *)where, payload, block.size);
			adreno8_to_memory(where, block.size);
			wrong = memcmp((const void *)where, payload, block.size) != 0;
		}
		if (wrong) {
			printf("adreno8: %u: firmware readback failed at %#x, size %#x\n", adreno8_step, block.address, block.size);
			return false;
		}
		written += block.size;
	}
	printf("adreno8: %u: GMU firmware loaded: %#x bytes verified\n", adreno8_step, written);
	adreno8_fw_loaded = true;
	return true;
}

bool
adreno8_step_gmu_start(void)
{
	uint32_t counter;
	if (!adreno8_cx_ready()) {
		return false;
	}
	if (!adreno8_fw_loaded || !adreno8_gmu_space.is_on || !adreno8_gmu_log.memory || !adreno8_gmu_hfi.memory ||
	    (!adreno8_direct_ring && !adreno8_gmu_vrb) || adreno8_gmu_runs || !(adreno8_rd(adreno8_gmu.base + GMU_CM3_SYSRESET) & 1)) {
		printf("adreno8: %u: GMU start requires verified firmware, mapped memory, asserted CM3 reset and the selected HFI memory\n", adreno8_step);
		return false;
	}
	/* Mainline ordinary HFI does not publish a virtual register bank. */
	if (adreno8_direct_ring && adreno8_get(&adreno8_gmu, "ordinary HFI GENERAL11", GMU_GENERAL_11) != 0) {
		printf("adreno8: ordinary HFI requires GENERAL11 zero; CM3 remains in reset\n");
		return false;
	}
	if (!adreno8_scm_gpu_init()) {
		return false;
	}
	/* This diagnostic polls status; no GMU interrupt handler is installed. */
	adreno8_put(&adreno8_gmu, "ao interrupt mask", GMU_AO_INTERRUPT_MASK, ~0u);
	adreno8_put(&adreno8_gmu, "gmu2host interrupt mask", GMU_GMU2HOST_MASK, ~0u);
	adreno8_put(&adreno8_gmu, "ao interrupt clear", GMU_AO_INTERRUPT_CLR, ~0u);
	adreno8_put(&adreno8_gmu, "gmu2host interrupt clear", GMU_GMU2HOST_CLR, ~0u);

	if (adreno8_rsc_asleep_at_first) {
		uint32_t req = adreno8_get(&adreno8_gmu, "rscc control req", GMU_RSCC_CONTROL_REQ);

		adreno8_rsc_woken_by_us = !(req & 2);
		adreno8_put(&adreno8_gmu, "request RSC wake", GMU_RSCC_CONTROL_REQ, req | 2);
		if (!adreno8_wait_for(&adreno8_gmu, GMU_RSCC_CONTROL_ACK, 2, 2, 100) ||
		    !adreno8_wait_for(&adreno8_rscc, RSCC_SEQ_BUSY, ~0u, 0, 100)) {
			printf("adreno8: %u: RSC wake failed: ack %#x, busy %#x\n", adreno8_step,
			    adreno8_rd(adreno8_gmu.base + GMU_RSCC_CONTROL_ACK), adreno8_rd(adreno8_rscc.base + RSCC_SEQ_BUSY));
			return false;
		}
		if (adreno8_rsc_woken_by_us) {
			adreno8_put(&adreno8_gmu, "restore RSC request after wake", GMU_RSCC_CONTROL_REQ, req);
			adreno8_rsc_woken_by_us = false;
		}
	}

	/* gen8_gmu_register_config; BCL remains disabled. */
	adreno8_put(&adreno8_gmu, "initial RPMh power state", GMU_RPMH_POWER_STATE, 0xf);
	adreno8_put(&adreno8_gmu, "falnext intf", GMU_CX_FALNEXT_INTF, 1);
	adreno8_put(&adreno8_gmu, "fal intf", GMU_CX_FAL_INTF, 1);
	adreno8_put(&adreno8_gmu, "clear firmware init result", GMU_FW_INIT_RESULT, 0);
	adreno8_put(&adreno8_gmu, "cm3 boot config", GMU_CM3_BOOT_CONFIG, 0x02);
	adreno8_put(&adreno8_gmu, "HFI queue table address", GMU_HFI_QTBL_ADDR, adreno8_gmu_hfi.address);
	adreno8_put(&adreno8_gmu, "HFI queue table info", GMU_HFI_QTBL_INFO, 1);
	adreno8_put(&adreno8_gmu, "AHB fence range", GMU_AHB_FENCE_RANGE_0, 1u << 31 | 0x32u << 18 | (adreno8_direct_ring ? 0x8c0u : 0x8a0u));
	adreno8_put(&adreno8_gmu, "cm3 config", GMU_CM3_CFG, 0x4052);
	adreno8_put(&adreno8_gpu, "gbif qsb side 0", GPU_GBIF_QSB_SIDE0, GPU_GBIF_QSB_VALUE);
	adreno8_put(&adreno8_gpu, "gbif qsb side 1", GPU_GBIF_QSB_SIDE0 + 4, GPU_GBIF_QSB_VALUE);
	adreno8_put(&adreno8_gpu, "gbif qsb side 2", GPU_GBIF_QSB_SIDE0 + 8, GPU_GBIF_QSB_VALUE);
	adreno8_put(&adreno8_gpu, "gbif qsb side 3", GPU_GBIF_QSB_SIDE0 + 12, GPU_GBIF_QSB_VALUE);
	adreno8_put(&adreno8_gpu, "gbif cx config", GPU_GBIF_CX_CONFIG, GPU_GBIF_CX_VALUE);
	adreno8_put(&adreno8_gmu, "GMU GBIF QoS", GMU_MRC_GBIF_QOS, 0x33);
	adreno8_put(&adreno8_gpu, "clear GBIF halt", GPU_GBIF_HALT, 1u << 3);
	if (!adreno8_direct_ring) {
		adreno8_put(&adreno8_gmu, "GMU virtual register bank", GMU_GENERAL_11, adreno8_gmu_vrb->gmu_address);
		adreno8_settle();
		if (adreno8_get(&adreno8_gmu, "GMU virtual register bank readback", GMU_GENERAL_11) != adreno8_gmu_vrb->gmu_address) {
			printf("adreno8: GMU VRB address readback failed; CM3 remains in reset\n");
			return false;
		}
	}
	adreno8_put(&adreno8_gmu, "clear log write index", GMU_GENERAL_9, 0);
	/* annibale's live DT identifies gen8_0_1 (chip ID 0x44050001).
	 * Vendor ADRENO_GMU_REV encodes major/minor/patch in bits 31:8. */
	adreno8_put(&adreno8_gmu, "gen8.0.1 firmware revision", GMU_GENERAL_10, 0x08000100);
	adreno8_put(&adreno8_gmu, "log address and page count", GMU_GENERAL_8,
	    (adreno8_gmu_log.address & ~0xfff) | ((adreno8_gmu_log.size / 4096 - 1) & 0xff));
	adreno8_put(&adreno8_gmu, "sys bus config", GMU_SYS_BUS_CONFIG, 1);
	adreno8_put(&adreno8_gmu, "icache config", GMU_ICACHE_CONFIG, 1);
	adreno8_put(&adreno8_gmu, "dcache config", GMU_DCACHE_CONFIG, 1);
	printf("adreno8: %u: using inherited DDR votes; host RPMh votes unchanged\n", adreno8_step);

	/* Cleanup must reset CM3 even if firmware initialization times out. */
	adreno8_gmu_runs = true;
	counter = adreno8_rd(adreno8_gmu.base + GMU_ALWAYS_ON_LO);
	adreno8_put(&adreno8_gmu, "release CM3 reset", GMU_CM3_SYSRESET, 0);
	adreno8_settle();
	if (!adreno8_wait_for(&adreno8_gmu, GMU_FW_INIT_RESULT, GMU_FW_STARTED_MASK, GMU_FW_STARTED, 100)) {
		printf("adreno8: %u: GMU initialization timed out: result %#x, AO counter delta %u\n", adreno8_step,
		    adreno8_rd(adreno8_gmu.base + GMU_FW_INIT_RESULT), adreno8_rd(adreno8_gmu.base + GMU_ALWAYS_ON_LO) - counter);
		adreno8_gmu_post_mortem();
		return false;
	}
	printf("adreno8: %u: GMU firmware started: result %#x, log %#x\n", adreno8_step,
	    adreno8_rd(adreno8_gmu.base + GMU_FW_INIT_RESULT), adreno8_gmu_log.address);
	return true;
}
