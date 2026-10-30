/*
 * adreno8_smmu.c -- the SMMU the GMU speaks through: its layout, the GMU's address space and its translation context: step 8 and the machinery steps 9 to 16 live in
 * The ladder, the boot-args and what is not settled: see adreno8.c; the shared world: adreno8_private.h.
 */
#include "hakios.adreno8_smmu_private.h"

#define printf adreno8_say

/* ---- step 8: the SMMU in front of the GPU ---------------------------------------------------------------- */

/*
 * The device tree assigns SID 5 with mask 0 to the GMU on kgsl-smmu@3da0000.
 * The vendor driver reserves contexts 0/1 for GPU/LPAC and allocates other clients
 * from context 2. Register-bank offsets follow the SMMUv2 ID registers.
 */

struct adreno8_block adreno8_smmu = { "smmu", 0x03da0000, 0x40000 };

uint32_t adreno8_smmu_page, adreno8_smmu_pages;
static uint32_t adreno8_smmu_contexts, adreno8_smmu_matches;

/* The layout of the register space from the ID registers: printed; only the context's room is required. */

static bool
adreno8_smmu_layout(void)
{
	uint32_t idr0 = adreno8_get(&adreno8_smmu, "idr0", SMMU_IDR0), idr1 = adreno8_get(&adreno8_smmu, "idr1", SMMU_IDR1);

	if (adreno8_direct_ring && (idr0 == ~0u || idr1 == ~0u)) return false;

	adreno8_smmu_page = idr1 >> 31 ? 0x10000 : 0x1000;
	adreno8_smmu_pages = 2u << (idr1 >> 28 & 7);
	adreno8_smmu_contexts = idr1 & 0xff;
	adreno8_smmu_matches = idr0 & 0xff;
	printf("adreno8: %u: SMMU: %u stream match registers, %u contexts, %u pages of %#x before the contexts' own\n", adreno8_step,
	    adreno8_smmu_matches, adreno8_smmu_contexts, adreno8_smmu_pages, adreno8_smmu_page);
	if (2 * adreno8_smmu_pages * adreno8_smmu_page != adreno8_smmu.size) {
		printf("adreno8: %u: that makes %#x of registers, the device tree says %#x: believed anyway (redwood's formula, an open question here)\n",
		    adreno8_step, 2 * adreno8_smmu_pages * adreno8_smmu_page, adreno8_smmu.size);
	}
	if (adreno8_smmu_contexts <= GMU_CONTEXT ||
	    (adreno8_smmu_pages + GMU_CONTEXT + 1) * adreno8_smmu_page > adreno8_smmu.size) {
		printf("adreno8: %u: GMU context %u is outside the SMMU register layout\n", adreno8_step, GMU_CONTEXT);
		return false;
	}
	return true;
}

/* Diagnostic-only context-bank dump (boot-args adreno8_cbscan=1). Reads a
 * few context banks so the log shows whether one holds a configuration, is
 * uninitialised, or cannot be read at all; the values never enter a decision
 * and no register is written. Contexts are grouped to answer three questions:
 * 0..3 are the vendor-reserved and own contexts named at step 8, 4 and 5 are
 * implemented but unused by HLOS, and 20 is the last implemented context.
 * Called twice: after the step-8 read and again once step 9 has configured the
 * GMU context, so the second dump carries a context bank whose write is known
 * to have landed and reads CB3 at the state the step-14 preflight judges. */
static uint32_t adreno8_cbscan_asked;
static bool adreno8_cbscan_done;   /* the step-8 dump, once: that read runs more than once */

void
adreno8_smmu_cb_scan_ask(void)
{
	adreno8_cbscan_asked = 1;
}

static void
adreno8_cb_scan_one(uint32_t cb, const char *role)
{
	uint32_t base;

	if (cb >= adreno8_smmu_contexts) {
		printf("adreno8: %u: cbscan context %u (%s): not implemented in this SMMU (%u contexts); skipped\n",
		    adreno8_step, cb, role, adreno8_smmu_contexts);
		return;
	}
	base = (adreno8_smmu_pages + cb) * adreno8_smmu_page;
	if ((adreno8_smmu_pages + cb + 1) * adreno8_smmu_page > adreno8_smmu.size) {
		printf("adreno8: %u: cbscan context %u (%s) at %#x: past the mapped SMMU block (%u bytes); skipped\n",
		    adreno8_step, cb, role, base, adreno8_smmu.size);
		return;
	}
	printf("adreno8: %u: cbscan context %u (%s) at %#x\n", adreno8_step, cb, role, base);
	adreno8_get(&adreno8_smmu, "cbscan sctlr", base + SMMU_CB_SCTLR);
	adreno8_get(&adreno8_smmu, "cbscan ttbr0, low", base + SMMU_CB_TTBR0);
	adreno8_get(&adreno8_smmu, "cbscan ttbr0, high", base + SMMU_CB_TTBR0 + 4);
	adreno8_get(&adreno8_smmu, "cbscan tcr", base + SMMU_CB_TCR);
	adreno8_get(&adreno8_smmu, "cbscan fsr", base + SMMU_CB_FSR);
}

static void
adreno8_cb_scan(const char *when)
{
	volatile uint32_t spin;

	adreno8_announce("cbscan (%s): read-only context-bank dump; no register is written and no value is judged", when);
	adreno8_cb_scan_one(0, "GPU SID 0, vendor reserved");
	adreno8_cb_scan_one(1, "LPAC SID 1, vendor reserved");
	adreno8_cb_scan_one(GMU_CONTEXT, "GMU, ours; 0x27 once step 9 has run");
	adreno8_cb_scan_one(SECURE_CONTEXT, "gfx3d_secure candidate");
	adreno8_cb_scan_one(4, "implemented, unused by HLOS");
	adreno8_cb_scan_one(5, "implemented, unused by HLOS");
	adreno8_cb_scan_one(20, "last implemented context, boundary");

	/* The same register a second time after a short delay: a repeatable value
	 * supports "programmed by someone", a moving one supports "not readable". */
	if (SECURE_CONTEXT < adreno8_smmu_contexts) {
		uint32_t base = (adreno8_smmu_pages + SECURE_CONTEXT) * adreno8_smmu_page;

		if ((adreno8_smmu_pages + SECURE_CONTEXT + 1) * adreno8_smmu_page <= adreno8_smmu.size) {
			for (spin = 0; spin < 400000u; spin++) {
			}
			adreno8_get(&adreno8_smmu, "cbscan sctlr, second read", base + SMMU_CB_SCTLR);
		}
	}
}

/* Called once step 9 has written the GMU context's SCTLR: from here the dump
 * carries a context bank whose configuration is known to have taken effect. */
void
adreno8_smmu_cb_scan_after_step9(void)
{
	if (!adreno8_cbscan_asked) {
		return;
	}
	adreno8_cb_scan("post-step-9");
}

/* Early ordinary-mode diagnostic, before this driver enables any context.
 * NSNH covers unlocked Non-secure non-Hyp entries and may over-invalidate
 * (Arm IHI 0062D.c 9.6.29). This scan covers only SMRs visible to HLOS;
 * secure-world reservations stay outside its checks. No route/context or Hyp write.
 * Fixed MiCode arm-smmu.c writes QCOM_DUMMY_VAL to both WO commands. */
static bool
adreno8_smmu_early_ns_tlb(void)
{
	uint32_t reset, gx, status, scr0;
	uint64_t start, elapsed;

	if (adreno8_gmu_space.is_on || adreno8_gpu_space.is_on || adreno8_secure_space.is_on) {
		printf("adreno8: %u: early NS TLB invalidation requires all owned contexts off\n", adreno8_step);
		return false;
	}
	reset = adreno8_get(&adreno8_gmu, "early NS TLB CM3 reset", GMU_CM3_SYSRESET);
	gx = adreno8_get(&adreno8_gmu, "early NS TLB GX status", GMU_PWR_CLK_STATUS);
	if (reset == ~0u || !(reset & 1) || gx == ~0u || (gx & 3) != 3) {
		printf("adreno8: %u: early NS TLB invalidation requires CM3 reset and GX off\n", adreno8_step);
		return false;
	}
	for (uint32_t i = 0; i < adreno8_smmu_matches; i++) {
		if (adreno8_get(&adreno8_smmu, "early NS TLB SMR", SMMU_SMR(i)) & SMR_VALID) {
			printf("adreno8: %u: early NS TLB invalidation rejected visible match %u\n", adreno8_step, i);
			return false;
		}
	}
	/* Arm IHI 0062D.c 9.6.1: GFIE is bit 2; keep GFRE and every other bit. */
	scr0 = adreno8_get(&adreno8_smmu, "early NS fault control", SMMU_SCR0);
	if (scr0 == ~0u || (scr0 & 1) || !(scr0 & 2)) {
		printf("adreno8: %u: early NS fault control rejected %#x\n", adreno8_step, scr0);
		return false;
	}
	if (scr0 & 4) {
		adreno8_reset_required = true;
		adreno8_put(&adreno8_smmu, "early NS global fault IRQ mask", SMMU_SCR0, scr0 & ~4u);
		adreno8_settle();
		if (adreno8_get(&adreno8_smmu, "early NS global fault IRQ readback", SMMU_SCR0) != (scr0 & ~4u))
			return false;
	}
	adreno8_reset_required = true;
	adreno8_settle();
	adreno8_put(&adreno8_smmu, "early NS TLB invalidate", SMMU_TLBIALLNSNH, ~0u);
	adreno8_settle();
	adreno8_put(&adreno8_smmu, "early NS TLB sync", SMMU_TLBGSYNC, ~0u);
	start = mach_absolute_time();
	for (;;) {
		status = adreno8_rd(adreno8_smmu.base + SMMU_TLBGSTATUS);
		if (status == ~0u) break;
		if (!(status & 1)) {
			adreno8_settle();
			printf("adreno8: %u: early NS TLB global sync completed, status %#x\n", adreno8_step, status);
			return true;
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= 100000000ull) break;
	}
	printf("adreno8: %u: early NS TLB global sync failed, status %#x\n", adreno8_step, status);
	return false;
}

bool
adreno8_step_smmu_read(void)
{
	if (!adreno8_cx_ready()) {
		return false;
	}
	adreno8_announce("read the ID registers of the GPU's SMMU (0x3da0000): global registers, which the hypervisor answers for");
	if (!adreno8_smmu_layout()) {
		return false;
	}
	adreno8_get(&adreno8_smmu, "idr2", SMMU_IDR2);
	adreno8_get(&adreno8_smmu, "idr7", SMMU_IDR7);
	adreno8_get(&adreno8_smmu, "scr0", SMMU_SCR0);
	adreno8_get(&adreno8_smmu, "sgfsr", SMMU_SGFSR);

	adreno8_announce("read the SMMU's stream table and the kinds of its contexts, as the boot loader and hypervisor left them");
	for (uint32_t i = 0; i < adreno8_smmu_matches && i < 16; i++) {
		adreno8_get(&adreno8_smmu, "smr", SMMU_SMR(i));
		adreno8_get(&adreno8_smmu, "s2cr", SMMU_S2CR(i));
	}
	for (uint32_t i = 0; i < adreno8_smmu_contexts && i < 16; i++) {
		adreno8_get(&adreno8_smmu, "cbar", adreno8_smmu_page + SMMU_CBAR(i));
	}

	/* Context 3 is the secure candidate: vendor sun-gpu.dtsi routes the
	 * gfx3d_secure stream as SID 2 through this SMMU. Reading its reset
	 * state is observation for the secure bring-up and issues no writes. */
	adreno8_announce("read GPU context 0, LPAC context 1, GMU context %u and secure candidate context %u",
	    GMU_CONTEXT, SECURE_CONTEXT);
	for (uint32_t cb = 0; cb <= SECURE_CONTEXT && cb < adreno8_smmu_contexts; cb++) {
		uint32_t base = (adreno8_smmu_pages + cb) * adreno8_smmu_page;

		/* adreno8_smmu_layout only proves the GMU context fits the mapped
		 * block; apply the preflight's bound to every context observed
		 * here so a bigger granule or page count cannot read past the end. */
		if ((adreno8_smmu_pages + cb + 1) * adreno8_smmu_page > adreno8_smmu.size) {
			printf("adreno8: %u: context %u starts past the mapped SMMU block: its reads are skipped\n",
			    adreno8_step, cb);
			break;
		}
		adreno8_get(&adreno8_smmu, "cb sctlr", base + SMMU_CB_SCTLR);
		adreno8_get(&adreno8_smmu, "cb ttbr0, low", base + SMMU_CB_TTBR0);
		adreno8_get(&adreno8_smmu, "cb ttbr0, high", base + SMMU_CB_TTBR0 + 4);
		adreno8_get(&adreno8_smmu, "cb tcr", base + SMMU_CB_TCR);
		adreno8_get(&adreno8_smmu, "cb fsr", base + SMMU_CB_FSR);
	}
	if (adreno8_cbscan_asked && !adreno8_cbscan_done) {
		adreno8_cbscan_done = true;
		adreno8_cb_scan("after step 8");
	}
	return !adreno8_direct_ring || adreno8_smmu_early_ns_tlb();
}

/* The 4 KiB granule and physical-range constants live in adreno8_private.h so
 * the MP_ASSIGN path checks pages against the same rules the mapper uses. */
#define SMMU_CB_MAIR1            0x3c
#define SMMU_CB_PAR              0x50
#define SMMU_CB_ATS1PR           0x800
#define SMMU_CB_ATSR             0x8f0
#define SMMU_IDR0_ATOSNS         (1u << 26)

struct adreno8_gpu_space adreno8_gmu_space = { "gmu", GMU_CONTEXT };
struct adreno8_gpu_space adreno8_gpu_space = { "gpu", 0 };
struct adreno8_gpu_space adreno8_secure_space = { "secure", SECURE_CONTEXT };

static bool adreno8_space_sync(struct adreno8_gpu_space *space);

/* A level-2 table is remembered by its index (leaf_slot) and checked again before it is trusted. */
static struct adreno8_space_table *
adreno8_space_table(struct adreno8_gpu_space *space, unsigned level, uint32_t index)
{
	uint16_t *slot = level == 2 && index < sizeof(space->leaf_slot) / sizeof(space->leaf_slot[0]) ?
	    &space->leaf_slot[index] : NULL;

	if (slot != NULL && *slot != 0 && *slot <= space->table_count &&
	    space->tables[*slot - 1].level == 2 && space->tables[*slot - 1].index == index) {
		return &space->tables[*slot - 1];
	}
	for (unsigned i = 0; i < space->table_count; i++) {
		if (space->tables[i].level == level && space->tables[i].index == index) {
			if (slot != NULL) {
				*slot = (uint16_t)(i + 1);
			}
			return &space->tables[i];
		}
	}
	return NULL;
}

static vm_offset_t
adreno8_space_add_table(struct adreno8_gpu_space *space, unsigned level, uint32_t index)
{
	struct adreno8_space_table *above = NULL, *t;

	if (level > 2 || space->table_count >= sizeof(space->tables) / sizeof(space->tables[0]) ||
	    PAGE_SIZE < SMMU_GRANULE || adreno8_space_table(space, level, index) != NULL) {
		return 0;
	}
	if (level == 0) {
		if (index != 0 || space->table_count != 0) {
			return 0;
		}
	} else {
		above = adreno8_space_table(space, level - 1, index >> 9);
		if (above == NULL || ((uint64_t *)above->table)[index & 0x1ff] != 0) {
			return 0;
		}
	}
	t = &space->tables[space->table_count];
	if (kmem_alloc(kernel_map, &t->table, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adreno8: %u: page-table allocation failed\n", adreno8_step);
		t->table = 0;
		return 0;
	}
	t->phys = kvtophys(t->table);
	if (t->phys == 0 || t->phys >= SMMU_PHYS_LIMIT || (t->phys & SMMU_GRANULE_MASK) != 0) {
		kmem_free(kernel_map, t->table, PAGE_SIZE);
		t->table = 0;
		return 0;
	}
	t->level = level;
	t->index = index;
	if (above != NULL) {
		/* A live walker must see the initialized child before its parent becomes valid. */
		if (space->is_on) {
			adreno8_to_memory(t->table, SMMU_GRANULE);
		}
		((uint64_t *)above->table)[index & 0x1ff] = t->phys | PTE_TABLE;
		if (space->is_on) {
			adreno8_to_memory(above->table, SMMU_GRANULE);
		}
	}
	space->table_count++;
	return t->table;
}

vm_offset_t
adreno8_space_new_table(struct adreno8_gpu_space *space, unsigned level, uint32_t index)
{
	return space->is_on ? 0 : adreno8_space_add_table(space, level, index);
}

uint64_t *
adreno8_space_entry(struct adreno8_gpu_space *space, uint32_t address)
{
	struct adreno8_space_table *leaf = adreno8_space_table(space, 2, address >> 21);
	return leaf == NULL ? NULL : &((uint64_t *)leaf->table)[address >> 12 & 0x1ff];
}

bool
adreno8_space_prepare_page(struct adreno8_gpu_space *space, uint32_t address)
{
	return (adreno8_space_table(space, 1, address >> 30) != NULL ||
	    adreno8_space_add_table(space, 1, address >> 30) != 0) &&
	    (adreno8_space_table(space, 2, address >> 21) != NULL ||
	    adreno8_space_add_table(space, 2, address >> 21) != 0);
}

/* What publishing has cost since the last line about it, written every 5 s while it goes on. */
static struct {
	uint32_t count;
	uint64_t since, bytes, cleaning, total;
} publish_cost;

static void
publish_note(uint64_t bytes, uint64_t began, uint64_t cleaned, uint64_t ended)
{
	uint64_t span, cleaning, total;

	publish_cost.count++;
	publish_cost.bytes += bytes;
	publish_cost.cleaning += cleaned - began;
	publish_cost.total += ended - began;
	if (publish_cost.since == 0) {
		publish_cost.since = began;
	}
	absolutetime_to_nanoseconds(ended - publish_cost.since, &span);
	if (span < 5 * NSEC_PER_SEC) {
		return;
	}
	absolutetime_to_nanoseconds(publish_cost.cleaning, &cleaning);
	absolutetime_to_nanoseconds(publish_cost.total, &total);
	printf("adreno8: page tables published %u times in %llu ms: %llu bytes cleaned, %llu us cleaning and %llu us in all on average\n",
	    publish_cost.count, span / NSEC_PER_MSEC, publish_cost.bytes / publish_cost.count, cleaning / 1000 / publish_cost.count,
	    total / 1000 / publish_cost.count);
	memset(&publish_cost, 0, sizeof(publish_cost));
}

static bool
publish_sync(struct adreno8_gpu_space *space, uint64_t bytes, uint64_t began)
{
	uint64_t cleaned = mach_absolute_time();
	bool quiet = adreno8_quiet, synced;

	/* (on every external buffer map and unmap while the GPU is in use: the TLB writes were announced at bring-up) */
	adreno8_quiet = true;
	synced = adreno8_space_sync(space);
	adreno8_quiet = quiet;
	publish_note(bytes, began, cleaned, mach_absolute_time());
	return synced;
}

bool
adreno8_space_publish(struct adreno8_gpu_space *space)
{
	uint64_t began = mach_absolute_time();

	if (!space->is_on) return false;
	for (unsigned i = 0; i < space->table_count; i++) {
		adreno8_to_memory(space->tables[i].table, SMMU_GRANULE);
	}
	return publish_sync(space, (uint64_t)space->table_count * SMMU_GRANULE, began);
}

/*
 * The leaf entries of whole pages [address, address + size) out of the cache, then the TLB
 * invalidation: what an external buffer's map or unmap changed. The tables the range needs
 * exist already; adreno8_space_add_table cleaned each one and its parent entry when it made them.
 */
bool
adreno8_space_publish_range(struct adreno8_gpu_space *space, uint64_t address, uint64_t size)
{
	uint64_t began = mach_absolute_time(), end = address + size;

	if (!space->is_on || ((address | size) & SMMU_GRANULE_MASK) != 0 || end > (1ULL << 32) || end < address) {
		return false;
	}
	for (uint64_t at = address, stop; at < end; at = stop) {
		uint64_t *entry = adreno8_space_entry(space, (uint32_t)at);

		stop = (at | ((1ULL << 21) - 1)) + 1;   /* the end of this leaf's 2 MB */
		stop = stop < end ? stop : end;
		if (entry == NULL) {
			return false;
		}
		adreno8_to_memory((vm_offset_t)entry, (vm_size_t)((stop - at) / SMMU_GRANULE * sizeof(*entry)));
	}
	return publish_sync(space, size / SMMU_GRANULE * sizeof(uint64_t), began);
}

bool
adreno8_space_find_range(struct adreno8_gpu_space *space, uint32_t start, uint32_t end,
    uint32_t size, uint32_t alignment, uint32_t *address)
{
	uint64_t candidate;

	if (address == NULL || size == 0 || (size & SMMU_GRANULE_MASK) ||
	    alignment < SMMU_GRANULE || (alignment & (alignment - 1)) || start >= end) {
		return false;
	}
	candidate = ((uint64_t)start + alignment - 1) & ~(uint64_t)(alignment - 1);
	while (candidate + size <= end) {
		uint32_t at;
		for (at = 0; at < size; at += SMMU_GRANULE) {
			uint64_t *entry = adreno8_space_entry(space, (uint32_t)candidate + at);
			if (entry != NULL && *entry != 0) {
				break;
			}
		}
		if (at == size) {
			*address = (uint32_t)candidate;
			return true;
		}
		candidate = (candidate + at + SMMU_GRANULE + alignment - 1) & ~(uint64_t)(alignment - 1);
	}
	return false;
}

bool
adreno8_space_map_region(struct adreno8_gpu_space *space, uint32_t address,
    vm_offset_t memory, uint32_t size, uint32_t prot)
{
	uint64_t attributes = adreno8_space_attributes(prot);

	if (!attributes || memory == 0 || size == 0 ||
	    ((address | memory | size) & SMMU_GRANULE_MASK) ||
	    (uint64_t)address + size > (1ULL << 32) || memory + size < memory ||
	    space->table_count == 0 || space->tables[0].level != 0) {
		return false;
	}
	/* Preflight every PA and existing PTE before publishing any data mapping. */
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE) {
		uint64_t phys = kvtophys(memory + at);
		uint64_t *entry = adreno8_space_entry(space, address + at);
		if (phys == 0 || phys >= SMMU_PHYS_LIMIT || (phys & SMMU_GRANULE_MASK) ||
		    (entry != NULL && *entry != 0)) {
			return false;
		}
	}
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE) {
		uint32_t va = address + at;
		if (!adreno8_space_prepare_page(space, va)) {
			return false;
		}
	}
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE) {
		*adreno8_space_entry(space, address + at) = kvtophys(memory + at) | attributes;
	}
	/* Only invalid-to-valid additions are supported while a context is running.
	 * The caller must retain backing memory on failure, including a TLB timeout. */
	if (space->is_on) {
		return adreno8_space_publish(space);
	}
	return true;
}

bool
adreno8_space_map(struct adreno8_gpu_space *space, uint32_t address, uint64_t phys)
{
	struct adreno8_space_table *leaf;
	uint32_t i0 = address >> 30, i01 = address >> 21;
	uint64_t *entry;

	if (space->is_on || ((address | phys) & SMMU_GRANULE_MASK) != 0 || phys == 0 || phys >= SMMU_PHYS_LIMIT) {
		return false;
	}
	if (adreno8_space_table(space, 1, i0) == NULL && adreno8_space_new_table(space, 1, i0) == 0) {
		return false;
	}
	leaf = adreno8_space_table(space, 2, i01);
	if (leaf == NULL) {
		if (adreno8_space_new_table(space, 2, i01) == 0) {
			return false;
		}
		leaf = adreno8_space_table(space, 2, i01);
	}
	entry = &((uint64_t *)leaf->table)[address >> 12 & 0x1ff];
	if (*entry != 0) {
		printf("adreno8: %u: duplicate IOVA %#x\n", adreno8_step, address);
		return false;
	}
	*entry = phys | PTE_PAGE;
	return true;
}

bool
adreno8_space_map_block(struct adreno8_gpu_space *space, const struct adreno8_gpu_block *b)
{
	uint64_t length = ((uint64_t)b->size + SMMU_GRANULE_MASK) & ~(uint64_t)SMMU_GRANULE_MASK;

	if (space->is_on || b->memory == 0 || b->size == 0 ||
	    ((b->address | b->memory) & SMMU_GRANULE_MASK) != 0 || (uint64_t)b->address + length > (1ULL << 32)) {
		return false;
	}
	/* Reject overlap before installing any part of this block. */
	for (uint64_t at = 0; at < length; at += SMMU_GRANULE) {
		uint32_t address = b->address + at;
		struct adreno8_space_table *leaf = adreno8_space_table(space, 2, address >> 21);

		if (leaf != NULL && ((uint64_t *)leaf->table)[address >> 12 & 0x1ff] != 0) {
			printf("adreno8: %u: block %s overlaps IOVA %#x\n", adreno8_step, b->name, address);
			return false;
		}
	}
	for (uint64_t at = 0; at < length; at += SMMU_GRANULE) {
		if (!adreno8_space_map(space, b->address + at, kvtophys(b->memory + at))) {
			return false;
		}
	}
	printf("adreno8: %u: %s IOVA %#x: %u mapped 4 KiB pages; XNU page size %#x\n", adreno8_step,
	    b->name, b->address, (unsigned)(length / SMMU_GRANULE), (unsigned)PAGE_SIZE);
	return true;
}

static bool
adreno8_space_sync(struct adreno8_gpu_space *space)
{
	uint32_t cb = (adreno8_smmu_pages + space->context) * adreno8_smmu_page;

	/* Every leaf has nG set, so this ASID invalidation covers the whole space. */
	adreno8_settle();
	adreno8_put(&adreno8_smmu, "cb tlb asid", cb + SMMU_CB_TLBASID, space->context);
	adreno8_put(&adreno8_smmu, "cb tlb sync", cb + SMMU_CB_TLBSYNC, 1);
	if (!adreno8_wait_for(&adreno8_smmu, cb + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("adreno8: %u: context %u TLB sync timed out\n", adreno8_step, space->context);
		return false;
	}
	adreno8_settle();
	return true;
}

/* Use a 64-bit register write for AArch64 ATS1PR, as in arm-smmu. */
static void __attribute__((noinline))
adreno8_smmu_write64(vm_offset_t address, uint64_t value)
{
	__asm__ volatile ("str %0, [%1]" : : "r" (value), "r" (address) : "memory");
}

static bool
adreno8_space_verify(struct adreno8_gpu_space *space, bool atos)
{
	uint32_t cb = (adreno8_smmu_pages + space->context) * adreno8_smmu_page;
	unsigned pages = 0;

	for (unsigned i = 0; i < space->table_count; i++) {
		struct adreno8_space_table *t = &space->tables[i];
		if (t->level != 2) {
			continue;
		}
		struct adreno8_space_table *parent = adreno8_space_table(space, 1, t->index >> 9);
		if (parent == NULL || ((uint64_t *)parent->table)[t->index & 0x1ff] != (t->phys | PTE_TABLE) ||
		    ((uint64_t *)space->tables[0].table)[t->index >> 9] != (parent->phys | PTE_TABLE)) {
			return false;
		}
		for (unsigned entry = 0; entry < 512; entry++) {
			uint64_t pte = ((uint64_t *)t->table)[entry];
			uint32_t address = (t->index << 21) | (entry << 12);
			uint64_t par;

			if (pte == 0) {
				continue;
			}
			bool external = space == &adreno8_gpu_space && space->context == 0;
			if (external && pte == ADRENO8_PTE_EXTERNAL) continue;
			uint64_t attributes = pte & ~PTE_ADDRESS;
			if (external) attributes &= ~ADRENO8_PTE_EXTERNAL;
			if ((attributes & ~SMMU_PTE_VARIABLE) != SMMU_PTE_REQUIRED ||
			    (attributes & (3ULL << 8)) < (2ULL << 8) ||
			    (pte & PTE_ADDRESS) == 0 || (pte & PTE_ADDRESS) >= SMMU_PHYS_LIMIT) {
				return false;
			}
			pages++;
			if (!atos) {
				continue;
			}
			adreno8_smmu_write64(adreno8_smmu.base + cb + SMMU_CB_ATS1PR, address);
			adreno8_settle();
			if (!adreno8_wait_for(&adreno8_smmu, cb + SMMU_CB_ATSR, 1, 0, 1)) {
				printf("adreno8: %u: ATOS timed out for IOVA %#x\n", adreno8_step, address);
				return false;
			}
			par = adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_PAR);
			par |= (uint64_t)adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_PAR + 4) << 32;
			if ((par & 1) != 0 || (par & PTE_ADDRESS) != (pte & PTE_ADDRESS)) {
				printf("adreno8: %u: ATOS IOVA %#x: PAR %#llx, expected PA %#llx\n", adreno8_step,
				    address, par, pte & PTE_ADDRESS);
				return false;
			}
		}
	}
	printf("adreno8: %u: %u page mappings verified by %s\n", adreno8_step, pages, atos ? "ATOS" : "software table walk");
	return pages != 0;
}

/*
 * Confirm the GMU space's route still reads back exactly as adreno8_space_on
 * configured it: require the GMU space to be enabled, the GMU_MATCH slot to
 * read back SMR_VALID | SMR_MASK << 16 | GMU_STREAM, and the S2CR to be a
 * translate naming adreno8_gmu_space.context. The MP_ASSIGN SCM call goes
 * out as an SMC from this CPU, so this readback says nothing about the SCM
 * path or about secure resource ownership; it covers the MMIO view and the
 * completed initialization configuration only. A missing slot, a moved
 * route, or a changed type or context rejects the secure experiment; the
 * check reads two registers and writes nothing. The secure world's own
 * reservations stay invisible to this check like everywhere else in the
 * preflight.
 */
static bool
adreno8_gmu_route_matches(void)
{
	uint32_t smr, s2cr;

	if (!adreno8_gmu_space.is_on) {
		printf("adreno8: %u: secure preflight: GMU route check: the GMU space is not enabled, so the configured route is not there to read back\n", adreno8_step);
		return false;
	}
	if (GMU_MATCH >= adreno8_smmu_matches) {
		printf("adreno8: %u: secure preflight: GMU route check: match %u falls outside the probed match registers\n", adreno8_step, GMU_MATCH);
		return false;
	}
	smr = adreno8_get(&adreno8_smmu, "gmu route check", SMMU_SMR(GMU_MATCH));
	if (!(smr & SMR_VALID)) {
		printf("adreno8: %u: secure preflight: GMU route missing: match %u smr %#x is not valid\n", adreno8_step, GMU_MATCH, smr);
		return false;
	}
	if (smr != (SMR_VALID | SMR_MASK << 16 | GMU_STREAM)) {
		printf("adreno8: %u: secure preflight: GMU route moved: match %u smr %#x names stream %u mask %#x instead of stream %u mask %#x\n",
		    adreno8_step, GMU_MATCH, smr, smr & 0x7fff, smr >> 16 & 0x7fff, GMU_STREAM, SMR_MASK);
		return false;
	}
	s2cr = adreno8_get(&adreno8_smmu, "gmu route check", SMMU_S2CR(GMU_MATCH));
	if ((s2cr & 0x30000) != 0) {
		printf("adreno8: %u: secure preflight: GMU route type changed: match %u s2cr %#x is bypass or fault; translate expected\n",
		    adreno8_step, GMU_MATCH, s2cr);
		return false;
	}
	if ((s2cr & 0xff) != adreno8_gmu_space.context) {
		printf("adreno8: %u: secure preflight: GMU route context changed: match %u s2cr %#x names context %u instead of %u\n",
		    adreno8_step, GMU_MATCH, s2cr, s2cr & 0xff, adreno8_gmu_space.context);
		return false;
	}
	return true;
}

static bool
adreno8_space_available(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match, bool strict)
{
	uint32_t sctlr;

	/*
	 * Occupancy pre-check: reads only; the SMR/S2CR/context-bank registers
	 * carry no read side effects (vendor arm-smmu.c:239,688 read context
	 * bank SCTLR the same plain way). A secure-world reservation of the
	 * same context bank is not readable here and stays unconfirmed until
	 * a device run. TTBR and stream routing are written later by
	 * adreno8_space_on, never here. strict selects the secure-preflight
	 * rules (M bit plus routing) and returns inside its own pass; the
	 * ordinary takeover keeps the conditions the step14b device runs
	 * verified.
	 */
	if (space->context >= adreno8_smmu_contexts || match >= adreno8_smmu_matches || stream > 0x7fff ||
	    (adreno8_smmu_pages + space->context + 1) * adreno8_smmu_page > adreno8_smmu.size) {
		return false;
	}
	if (strict) {
		/*
		 * The strict pass returns inside this block after one read per
		 * register. It rejects the candidate when CB SCTLR.M is set, and
		 * the routing pass below rejects a valid candidate slot, a valid
		 * route covering the requested stream, or any valid route naming
		 * the context. The specification permits SCTLR to read UNKNOWN
		 * after reset, so fields other than M go uninterpreted; the
		 * device-read 0x96cc51a for example has a clear M bit.
		 */
		unsigned scanned = 0, valid = 0, hits = 0;
		uint64_t now = mach_absolute_time(), ns;
		bool m_set;

		sctlr = adreno8_get(&adreno8_smmu, "cb preflight",
		    (adreno8_smmu_pages + space->context) * adreno8_smmu_page + SMMU_CB_SCTLR);
		m_set = (sctlr & SCTLR_M) != 0;
		if (m_set) {
			printf("adreno8: %u: secure preflight: context %u SCTLR %#x has the M bit set; the context bank fails the preflight\n",
			    adreno8_step, space->context, sctlr);
		}
		/*
		 * Same-moment routing view, one read-only pass; values print as
		 * read, no second pass, no MMIO beyond one SMR per slot plus one
		 * S2CR per valid SMR. The candidate slot's SMR prints even when
		 * invalid, and then its S2CR is explicitly unread. Every valid
		 * entry prints (bounded by the probed match count), so
		 * post-analysis can check whether the kernel's own GMU route (a
		 * valid entry covering stream 5 with the S2CR pointing at
		 * context 2) shows up: all SMRs reading invalid is degraded
		 * read-out, a different picture from a clear one. Nothing in the
		 * view describes what the secure world has reserved. The step
		 * number and monotonic nanoseconds tag the moment.
		 */
		absolutetime_to_nanoseconds(now, &ns);
		for (unsigned i = 0; i < adreno8_smmu_matches; i++) {
			uint32_t smr = adreno8_get(&adreno8_smmu, "smr sample", SMMU_SMR(i));
			uint32_t s2cr = 0, mask;

			scanned++;
			if (!(smr & SMR_VALID)) {
				if (i == match) {
					printf("adreno8: %u: preflight routing sample: match %u smr %#x invalid; s2cr not read\n",
					    adreno8_step, i, smr);
				}
				continue;
			}
			s2cr = adreno8_get(&adreno8_smmu, "s2cr sample", SMMU_S2CR(i));
			valid++;
			printf("adreno8: %u: preflight routing sample: match %u smr %#x s2cr %#x\n",
			    adreno8_step, i, smr, s2cr);
			mask = (smr >> 16 & 0x7fff) | SMR_MASK;
			if (i == match || ((smr ^ stream) & 0x7fff & ~mask) == 0 ||
			    (s2cr & 0xff) == space->context) {
				hits++;
			}
		}
		printf("adreno8: %u: preflight routing sample at t=%lluns: scanned %u, valid %u, %u on the candidate route or context %u; %s\n",
		    adreno8_step, ns, scanned, valid, hits, space->context,
		    m_set ? "the M-bit rejection stands" :
		    hits != 0 ? "a routing conflict rejects the candidate" :
		    "HLOS-side conditions are satisfied at the sample time");
		if (m_set || hits != 0) {
			return false;
		}
		/* The candidate side passed; the GMU space's own route must
		 * still read back as it was configured. */
		return adreno8_gmu_route_matches();
	}
	for (unsigned i = 0; i < adreno8_smmu_matches; i++) {
		uint32_t smr = adreno8_get(&adreno8_smmu, "smr ownership", SMMU_SMR(i));
		uint32_t s2cr, mask;

		if (!(smr & SMR_VALID)) {
			continue;
		}
		s2cr = adreno8_get(&adreno8_smmu, "s2cr ownership", SMMU_S2CR(i));
		/* Reuse a blocked stream only with the same match and a disabled context. */
		if (i == match && smr == (SMR_VALID | SMR_MASK << 16 | stream) &&
		    (s2cr & 0x30000) == SMMU_S2CR_FAULT &&
		    adreno8_get(&adreno8_smmu, "cb ownership",
		        (adreno8_smmu_pages + space->context) * adreno8_smmu_page + SMMU_CB_SCTLR) == 0) {
			continue;
		}
		mask = (smr >> 16 & 0x7fff) | SMR_MASK;
		/* The ordinary takeover rejects a valid entry on the candidate match,
		 * a valid entry covering the requested stream, and an active route
		 * into the requested context, exactly as the step14b runs verified. */
		/* This phone's boot chain pre-configures the GMU's route exactly
		 * as this driver would write it (a valid SMR naming stream 5 with
		 * the configured mask, a translate S2CR into the requested
		 * context): adopt it. The write path rewrites the same values
		 * anyway, and its read-backs then verify the registers hold what
		 * the rest of the driver expects. */
		if (i == match && smr == (SMR_VALID | SMR_MASK << 16 | stream) &&
		    (s2cr & 0x30303ff) == (SMMU_S2CR_SHCFG_NSH | space->context)) {
			printf("adreno8: %u: stream match %u already carries this driver's own route (stream %#x -> context %u): adopted\n",
			    adreno8_step, i, stream, space->context);
			continue;
		}
		if (i == match || ((smr ^ stream) & 0x7fff & ~mask) == 0 ||
		    ((s2cr & 0x30000) == 0 && (s2cr & 0xff) == space->context)) {
			printf("adreno8: %u: stream match %u conflicts with requested context %u: smr %#x (valid %d, stream %#x, mask %#x), s2cr %#x (type %u -> context %u); the conflict is %s\n",
			    adreno8_step, i, space->context, smr, (smr & SMR_VALID) != 0, smr & 0x7fff, smr >> 16 & 0x7fff, s2cr,
			    (unsigned)(s2cr >> 16 & 3), (unsigned)(s2cr & 0xff),
			    i == match ? "a valid entry on the candidate match slot" :
			    ((smr ^ stream) & 0x7fff & ~mask) == 0 ? "an entry covering the requested stream" :
			    "a live route into the requested context");
			return false;
		}
	}
	return true;
}

/*
 * Read-only secure preflight, run once per kind-5 secure allocation before
 * that request's first MP_ASSIGN batch. The candidate route is stream 2,
 * match 2, context 3 (vendor sun-gpu.dtsi gfx3d_secure). Bounds are checked
 * before any MMIO read; the scan reads every probed SMR/S2CR pair (24 slots
 * on the reference, while the boot probe samples only the first 16) and the
 * context-bank registers inside the mapped range. A context SCTLR with M set, a valid SMR on the
 * candidate slot or covering stream 2, and an S2CR of any type pointing at
 * context 3 each fail the check. The check additionally requires the GMU's
 * own stream 5 route to read back exactly as adreno8_space_on configured it:
 * enabled GMU space, a valid GMU_MATCH slot naming stream 5 with the
 * configured mask, and a translate S2CR naming the GMU context. A missing,
 * moved, or wrong-typed GMU route rejects the experiment too. The pass
 * samples the probed SMR/S2CR range once at that moment and prints what
 * routes where; the sample never changes the verdict. A pass states
 * HLOS-side conditions at the sample time only: what the secure world has
 * reserved stays invisible, and it never writes.
 */
bool
adreno8_secure_preflight(void)
{
	if (SECURE_CONTEXT >= adreno8_smmu_contexts || SECURE_MATCH >= adreno8_smmu_matches ||
	    SECURE_STREAM > 0x7fff ||
	    (adreno8_smmu_pages + SECURE_CONTEXT + 1) * adreno8_smmu_page > adreno8_smmu.size) {
		printf("adreno8: %u: secure candidate route falls outside the probed SMMU ranges\n", adreno8_step);
		return false;
	}
	if (!adreno8_space_available(&adreno8_secure_space, SECURE_STREAM, SECURE_MATCH, true)) {
		return false;
	}
	printf("adreno8: %u: secure preflight: stream %u match %u context %u satisfied the HLOS-side conditions at the sample time\n",
	    adreno8_step, SECURE_STREAM, SECURE_MATCH, SECURE_CONTEXT);
	return true;
}

static bool
adreno8_space_registers(struct adreno8_gpu_space *space)
{
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;
	uint32_t cb = (adreno8_smmu_pages + space->context) * adreno8_smmu_page;
	bool ok;

	/* TBI0/1 can read as one on MMU-500; all IOVAs here are untagged 32-bit values. */
	ok = adreno8_get(&adreno8_smmu, "cb sctlr", cb + SMMU_CB_SCTLR) == adreno8_smmu_sctlr(space->context);
	ok &= adreno8_get(&adreno8_smmu, "cb a2r", adreno8_smmu_page + SMMU_CBA2R(space->context)) == 1;
	ok &= adreno8_get(&adreno8_smmu, "cb bar", adreno8_smmu_page + SMMU_CBAR(space->context)) == STAGE_1;
	ok &= (adreno8_get(&adreno8_smmu, "cb tcr2", cb + SMMU_CB_TCR2) & ~SMMU_TCR2_TBI) == TCR2_39_BITS;
	uint32_t tcr = adreno8_get(&adreno8_smmu, "cb tcr", cb + SMMU_CB_TCR);
	/* After the CP and GMU run, device CB0 reads TCR with EPD1 (bit 23) clear; TTBR1 stays 0
	 * and every IOVA here is below the 39-bit TTBR0 limit, so that walk is never selected. */
	if (space == &adreno8_gpu_space && tcr == (TCR_39_BITS & ~(1u << 23))) {
		static bool reported;
		if (!reported)
			printf("adreno8: %u: GPU context TCR EPD1 was cleared after GPU execution (%#x)\n", adreno8_step, tcr);
		reported = true;
	} else {
		ok &= tcr == TCR_39_BITS;
	}
	ok &= adreno8_get(&adreno8_smmu, "cb ttbr0, low", cb + SMMU_CB_TTBR0) == (uint32_t)root;
	ok &= adreno8_get(&adreno8_smmu, "cb ttbr0, high", cb + SMMU_CB_TTBR0 + 4) == (uint32_t)(root >> 32);
	ok &= adreno8_get(&adreno8_smmu, "cb ttbr1, low", cb + SMMU_CB_TTBR1) == 0;
	ok &= adreno8_get(&adreno8_smmu, "cb ttbr1, high", cb + SMMU_CB_TTBR1 + 4) == 0;
	ok &= adreno8_get(&adreno8_smmu, "cb mair0", cb + SMMU_CB_MAIR0) == MAIR0_NORMAL;
	ok &= adreno8_get(&adreno8_smmu, "cb mair1", cb + SMMU_CB_MAIR1) == 0;
	if (adreno8_direct_ring && space == &adreno8_gpu_space && space->context == 0)
		ok &= adreno8_get(&adreno8_smmu, "cb actlr", cb + SMMU_CB_ACTLR) == SMMU_GPU_ACTLR;
	if (!ok) {
		printf("adreno8: %u: context %u register verification failed\n", adreno8_step, space->context);
	}
	return ok;
}

bool
adreno8_space_check(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match)
{
	if (space == NULL || space == &adreno8_secure_space || !space->is_on ||
	    space->table_count == 0 || space->table_count > sizeof(space->tables) / sizeof(space->tables[0]) ||
	    space->tables[0].level != 0 || space->secure_tables != 0 || adreno8_smmu_page == 0 ||
	    space->context >= adreno8_smmu_contexts || match >= adreno8_smmu_matches || stream > 0x7fff ||
	    (adreno8_smmu_pages + space->context + 1) * adreno8_smmu_page > adreno8_smmu.size) {
		return false;
	}
	bool ok = adreno8_space_registers(space);
	ok &= adreno8_get(&adreno8_smmu, "smr check", SMMU_SMR(match)) == (SMR_VALID | SMR_MASK << 16 | stream);
	ok &= (adreno8_get(&adreno8_smmu, "s2cr check", SMMU_S2CR(match)) & 0x30303ff) ==
	    (SMMU_S2CR_SHCFG_NSH | space->context);
	ok &= adreno8_space_verify(space, false);
	ok &= !adreno8_space_faulted(space);
	if (!ok) {
		printf("adreno8: %u: %s active translation verification failed; configuration retained\n", adreno8_step, space->name);
	}
	return ok;
}

bool
adreno8_space_on(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match)
{
	uint64_t root;
	uint32_t cb, idr0, idr1, idr2, smr = SMR_VALID | SMR_MASK << 16 | stream;
	uint32_t s2cr = SMMU_S2CR_SHCFG_NSH | space->context;
	uint32_t sctlr = adreno8_smmu_sctlr(space->context);
	bool atos;

	adreno8_announce("configure %s SMMU context %u and verify its page mappings", space->name, space->context);
	if (space->is_on || space->table_count == 0 || space->tables[0].level != 0 || !adreno8_smmu_layout() ||
	    !adreno8_space_available(space, stream, match, false)) {
		return false;
	}
	idr0 = adreno8_get(&adreno8_smmu, "idr0 capabilities", SMMU_IDR0);
	idr1 = adreno8_get(&adreno8_smmu, "idr1 capabilities", SMMU_IDR1);
	idr2 = adreno8_get(&adreno8_smmu, "idr2 capabilities", SMMU_IDR2);
	if (!(idr0 & 1u << 30) || !(idr0 & 1u << 27) || !(idr2 & 1u << 12) ||
	    (idr2 & 15) < 1 || (idr2 >> 4 & 15) < 1 || (idr2 >> 8 & 15) < 2 ||
	    space->context < (idr1 >> 16 & 0xff) || (adreno8_get(&adreno8_smmu, "scr0", SMMU_SCR0) & 1)) {
		printf("adreno8: %u: SMMU does not support the requested active 39-bit stage-1 context\n", adreno8_step);
		return false;
	}
	atos = !(idr0 & SMMU_IDR0_ATOSNS);
	if (!atos) {
		printf("adreno8: %u: IDR0.ATOSNS=1; hardware translation operations unavailable\n", adreno8_step);
	}
	if (!adreno8_space_verify(space, false)) {
		return false;
	}
	for (unsigned i = 0; i < space->table_count; i++) {
		adreno8_to_memory(space->tables[i].table, SMMU_GRANULE);
	}
	root = space->tables[0].phys | (uint64_t)space->context << 48;
	cb = (adreno8_smmu_pages + space->context) * adreno8_smmu_page;

	/* is_on also retains ownership after a failed write until cleanup is confirmed. */
	space->is_on = true;
	adreno8_put(&adreno8_smmu, "s2cr fault", SMMU_S2CR(match), SMMU_S2CR_FAULT | SMMU_S2CR_SHCFG_NSH);
	adreno8_put(&adreno8_smmu, "smr", SMMU_SMR(match), smr);
	if ((adreno8_get(&adreno8_smmu, "s2cr fault", SMMU_S2CR(match)) & 0x30000) != SMMU_S2CR_FAULT ||
	    adreno8_get(&adreno8_smmu, "smr", SMMU_SMR(match)) != smr) {
		goto fail;
	}
	adreno8_put(&adreno8_smmu, "cb sctlr", cb + SMMU_CB_SCTLR, 0);
	if (adreno8_get(&adreno8_smmu, "cb sctlr disabled", cb + SMMU_CB_SCTLR) != 0) {
		goto fail;
	}
	if (adreno8_direct_ring && space == &adreno8_gpu_space && space->context == 0) {
		adreno8_put(&adreno8_smmu, "cb actlr", cb + SMMU_CB_ACTLR, SMMU_GPU_ACTLR);
		adreno8_settle();
		if (adreno8_get(&adreno8_smmu, "cb actlr readback", cb + SMMU_CB_ACTLR) != SMMU_GPU_ACTLR)
			goto fail;
	}
	adreno8_put(&adreno8_smmu, "cb a2r", adreno8_smmu_page + SMMU_CBA2R(space->context), 1);
	adreno8_put(&adreno8_smmu, "cb bar", adreno8_smmu_page + SMMU_CBAR(space->context), STAGE_1);
	adreno8_put(&adreno8_smmu, "cb tcr2", cb + SMMU_CB_TCR2, TCR2_39_BITS);
	adreno8_put(&adreno8_smmu, "cb tcr", cb + SMMU_CB_TCR, TCR_39_BITS);
	adreno8_put(&adreno8_smmu, "cb ttbr0, low", cb + SMMU_CB_TTBR0, (uint32_t)root);
	adreno8_put(&adreno8_smmu, "cb ttbr0, high", cb + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	adreno8_put(&adreno8_smmu, "cb ttbr1, low", cb + SMMU_CB_TTBR1, 0);
	adreno8_put(&adreno8_smmu, "cb ttbr1, high", cb + SMMU_CB_TTBR1 + 4, 0);
	adreno8_put(&adreno8_smmu, "cb mair0", cb + SMMU_CB_MAIR0, MAIR0_NORMAL);
	adreno8_put(&adreno8_smmu, "cb mair1", cb + SMMU_CB_MAIR1, 0);
	adreno8_put(&adreno8_smmu, "cb fsr", cb + SMMU_CB_FSR, FSR_ALL);
	if (!adreno8_space_sync(space)) {
		goto fail;
	}
	adreno8_put(&adreno8_smmu, "cb sctlr", cb + SMMU_CB_SCTLR, sctlr);
	adreno8_settle();
	if (!adreno8_space_registers(space)) {
		goto fail;
	}
	if (atos && !adreno8_space_verify(space, true)) {
		goto fail;
	}
	adreno8_put(&adreno8_smmu, "s2cr translate", SMMU_S2CR(match), s2cr);
	adreno8_settle();
	if ((adreno8_get(&adreno8_smmu, "s2cr", SMMU_S2CR(match)) & 0x30303ff) != s2cr ||
	    adreno8_space_faulted(space)) {
		goto fail;
	}
	if (space == &adreno8_gpu_space && !adreno8_direct_ring) {
		/* Compare the inherited CB0 ACTLR with the vendor's per-stream configuration. */
		adreno8_get(&adreno8_smmu, "gpu cb actlr", cb + SMMU_CB_ACTLR);
	}
	printf("adreno8: %u: %s translation enabled: stream %u, context %u, root PA %#llx\n", adreno8_step,
	    space->name, stream, space->context, space->tables[0].phys);
	return true;
fail:
	adreno8_space_off(space, match);
	return false;
}

bool
adreno8_space_route_check(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match)
{
	return space->is_on && match < adreno8_smmu_matches &&
	    adreno8_get(&adreno8_smmu, "route smr check", SMMU_SMR(match)) == (SMR_VALID | SMR_MASK << 16 | stream) &&
	    (adreno8_get(&adreno8_smmu, "route s2cr check", SMMU_S2CR(match)) & 0x30303ff) ==
	    (SMMU_S2CR_SHCFG_NSH | space->context);
}

/*
 * Route one more stream into an enabled context. The match slot must be unused and
 * no valid entry may cover the stream; a failed step leaves the slot faulting.
 */
bool
adreno8_space_route(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match)
{
	uint32_t smr = SMR_VALID | SMR_MASK << 16 | stream;

	if (!space->is_on || match >= adreno8_smmu_matches || stream > 0x7fff)
		return false;
	for (uint32_t i = 0; i < adreno8_smmu_matches; i++) {
		uint32_t other = adreno8_get(&adreno8_smmu, "route smr ownership", SMMU_SMR(i));
		uint32_t mask = (other >> 16 & 0x7fff) | SMR_MASK;

		if ((other & SMR_VALID) && (i == match || ((other ^ stream) & 0x7fff & ~mask) == 0)) {
			printf("adreno8: %u: stream %u route conflicts with match %u (%#x)\n", adreno8_step, stream, i, other);
			return false;
		}
	}
	adreno8_put(&adreno8_smmu, "route s2cr fault", SMMU_S2CR(match), SMMU_S2CR_FAULT | SMMU_S2CR_SHCFG_NSH);
	adreno8_put(&adreno8_smmu, "route smr", SMMU_SMR(match), smr);
	adreno8_settle();
	if ((adreno8_get(&adreno8_smmu, "route s2cr fault", SMMU_S2CR(match)) & 0x30000) != SMMU_S2CR_FAULT ||
	    adreno8_get(&adreno8_smmu, "route smr", SMMU_SMR(match)) != smr)
		return false;
	adreno8_put(&adreno8_smmu, "route s2cr translate", SMMU_S2CR(match), SMMU_S2CR_SHCFG_NSH | space->context);
	adreno8_settle();
	if (!adreno8_space_route_check(space, stream, match) || adreno8_space_faulted(space)) {
		adreno8_put(&adreno8_smmu, "route s2cr fault", SMMU_S2CR(match), SMMU_S2CR_FAULT | SMMU_S2CR_SHCFG_NSH);
		return false;
	}
	printf("adreno8: %u: %s translation route added: stream %u, match %u, context %u\n", adreno8_step,
	    space->name, stream, match, space->context);
	return true;
}

void
adreno8_space_off(struct adreno8_gpu_space *space, uint32_t match)
{
	uint32_t cb = (adreno8_smmu_pages + space->context) * adreno8_smmu_page;
	bool stopped;

	if (!space->is_on) {
		return;
	}
	/* Keep the match valid: unmatched streams may bypass the SMMU. */
	adreno8_put(&adreno8_smmu, "s2cr fault", SMMU_S2CR(match), SMMU_S2CR_FAULT | SMMU_S2CR_SHCFG_NSH);
	adreno8_put(&adreno8_smmu, "cb sctlr", cb + SMMU_CB_SCTLR, 0);
	adreno8_settle();
	stopped = (adreno8_get(&adreno8_smmu, "s2cr fault", SMMU_S2CR(match)) & 0x30000) == SMMU_S2CR_FAULT &&
	    adreno8_get(&adreno8_smmu, "cb sctlr", cb + SMMU_CB_SCTLR) == 0;
	if (!(adreno8_rd(adreno8_smmu.base + SMMU_IDR0) & SMMU_IDR0_ATOSNS) &&
	    !adreno8_wait_for(&adreno8_smmu, cb + SMMU_CB_ATSR, 1, 0, 1)) {
		stopped = false;
	}
	if (!adreno8_space_sync(space) || !stopped) {
		printf("adreno8: %u: context %u cleanup incomplete; retain all mapped memory\n", adreno8_step, space->context);
		return;
	}
	space->is_on = false;
}

bool
adreno8_space_faulted(struct adreno8_gpu_space *space)
{
	uint32_t cb = (adreno8_smmu_pages + space->context) * adreno8_smmu_page, fsr;

	fsr = adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_FSR) & FSR_ALL;
	if (fsr == 0) {
		return false;
	}
	printf("adreno8: %u: %s SMMU fault %#x at %#x:%#x, syndrome %#x\n", adreno8_step, space->name, fsr,
	    adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_FAR_HI), adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_FAR_LO),
	    adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_FSYNR0));
	/* Secure diagnostics read registers only. Ownership transfer forbids CPU
	 * access to data pages; this path also leaves shared table contents untouched. */
	if (space == &adreno8_secure_space) {
		printf("adreno8: %u: secure fault context SCTLR %#x, global FSR %#x\n", adreno8_step,
		    adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_SCTLR),
		    adreno8_rd(adreno8_smmu.base + SMMU_SGFSR));
		return true;
	}
	uint32_t address = adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_FAR_LO);
	struct adreno8_space_table *leaf = adreno8_space_table(space, 2, address >> 21);
	printf("adreno8: %u: fault context SCTLR %#x, leaf PTE %#llx, global FSR %#x\n", adreno8_step,
	    adreno8_rd(adreno8_smmu.base + cb + SMMU_CB_SCTLR),
	    leaf != NULL ? ((uint64_t *)leaf->table)[address >> 12 & 0x1ff] : 0,
	    adreno8_rd(adreno8_smmu.base + SMMU_SGFSR));
	return true;
}
