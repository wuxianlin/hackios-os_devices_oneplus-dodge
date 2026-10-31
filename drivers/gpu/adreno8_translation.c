/* Validate complete object mappings before secure handover or CP startup. */
#include "adreno8_smmu_private.h"

#define printf adreno8_say

/* Resolve descriptors only to table allocations owned by this address space.
 * A descriptor with extra attributes is rejected: this mapper emits PA | 3. */
static const struct adreno8_space_table *
adreno8_translation_table(const struct adreno8_gpu_space *space, uint64_t descriptor,
    unsigned level, uint32_t index)
{
	for (unsigned i = 0; i < space->table_count; i++) {
		const struct adreno8_space_table *table = &space->tables[i];

		if (table->level == level && table->index == index && table->table != 0 &&
		    table->phys != 0 && table->phys < SMMU_PHYS_LIMIT &&
		    (table->phys & SMMU_GRANULE_MASK) == 0 &&
		    kvtophys(table->table) == table->phys && descriptor == (table->phys | PTE_TABLE)) {
			return table;
		}
	}
	return NULL;
}

/* The root and both intermediate descriptors are checked for each 4 KiB leaf.
 * Data pages can be physically discontiguous even within one allocation. */
bool
adreno8_translation_walk(struct adreno8_gpu_space *space, uint32_t iova,
    vm_offset_t memory, uint32_t size, uint32_t prot)
{
	uint64_t attributes = 3ULL | 2ULL << 8 | 1ULL << 10 | 1ULL << 11;
	const struct adreno8_space_table *root;

	if (space == NULL || !((space->context == SECURE_CONTEXT && !space->is_on) ||
	    (space == &adreno8_gpu_space && space->context == 0 && space->is_on)) ||
	    space->table_count == 0 || space->table_count > sizeof(space->tables) / sizeof(space->tables[0]) ||
	    !(prot & ADRENO8_MEM_READ) || (prot & ~15u) || memory == 0 || size == 0 ||
	    ((iova | memory | size) & SMMU_GRANULE_MASK) ||
	    (uint64_t)iova + size > (1ULL << 32) || memory + size < memory) {
		return false;
	}
	root = adreno8_translation_table(space, space->tables[0].phys | PTE_TABLE, 0, 0);
	if (root != &space->tables[0]) {
		return false;
	}
	if (!(prot & ADRENO8_MEM_WRITE)) attributes |= 1ULL << 7;
	if (!(prot & ADRENO8_MEM_PRIV)) attributes |= 1ULL << 6;
	if (!(prot & ADRENO8_MEM_EXEC)) attributes |= 3ULL << 53;
	for (uint64_t at = 0; at < size; at += SMMU_GRANULE) {
		uint32_t address = iova + at;
		uint64_t descriptor = ((const uint64_t *)root->table)[address >> 30];
		const struct adreno8_space_table *middle =
		    adreno8_translation_table(space, descriptor, 1, address >> 30);
		const struct adreno8_space_table *leaf = NULL;
		uint64_t pte = 0, phys = kvtophys(memory + at);

		if (middle != NULL) {
			descriptor = ((const uint64_t *)middle->table)[address >> 21 & 0x1ff];
			leaf = adreno8_translation_table(space, descriptor, 2, address >> 21);
		}
		if (leaf != NULL) {
			pte = ((const uint64_t *)leaf->table)[address >> 12 & 0x1ff];
		}
		if (leaf == NULL || phys == 0 || phys >= SMMU_PHYS_LIMIT ||
		    (phys & SMMU_GRANULE_MASK) || pte != (phys | attributes)) {
			printf("adreno8: translation walk failed at IOVA %#x: descriptor %#llx,"
			    " PTE %#llx, expected PA %#llx attributes %#llx\n",
			    address, descriptor, pte, phys, attributes);
			return false;
		}
	}
	printf("adreno8: translation walk verified %u bytes (%u leaves) from IOVA %#x"
	    " through root PA %#llx\n", size, size / SMMU_GRANULE, iova, root->phys);
	return true;
}

/*
 * Mode 3 (3a): point secure context 3 at the tables just handed over and switch it on,
 * leaving stream 2's route at fault so no transaction reaches it. The proofs a
 * write-restricted path can give are all here: every buffer page resolves through the
 * root table before the handover (see adreno8_translation_walk), every
 * write reads back, the TLB invalidate completes, and FSR is clear. This SMMU reports
 * IDR0.ATOSNS=1, so ATS1PR/ATSR cannot add a hardware translation proof; only the
 * firmware's own traffic in 3b can.
 *
 * A failed step stops here. Context 3's registers are not put back: the bank was not
 * enabled before this attempt, and a write-back could not prove the secure world never
 * used it, so the extra write buys nothing and touches a bank that may not be ours.
 */
static bool
adreno8_3a_write(uint32_t offset, uint32_t value, uint32_t *read_out, const char *name)
{
	adreno8_put(&adreno8_smmu, name, offset, value);
	*read_out = adreno8_get(&adreno8_smmu, name, offset);
	printf("adreno8: %u: 3a %s (%#x) write %#010x read %#010x\n", adreno8_step, name,
	    adreno8_smmu.phys + offset, value, *read_out);
	return *read_out == value;
}

static bool
adreno8_3a_stop(const char *name, uint32_t wrote, uint32_t read)
{
	printf("adreno8: %u: secure translation 3a stopped at %s: wrote %#010x, reads %#010x;"
	    " context %u retained; no further register writes\n", adreno8_step, name, wrote, read,
	    SECURE_CONTEXT);
	return false;
}

bool
adreno8_secure_translation_3a(struct adreno8_gpu_space *space)
{
	uint32_t cb = (adreno8_smmu_pages + SECURE_CONTEXT) * adreno8_smmu_page;
	uint64_t root;
	uint32_t read, value;

	if (space != &adreno8_secure_space || space->is_on || space->context != SECURE_CONTEXT ||
	    space->table_count == 0 || space->table_count > sizeof(space->tables) / sizeof(space->tables[0]) ||
	    space->tables[0].level != 0 || space->tables[0].index != 0) {
		return false;
	}
	root = space->tables[0].phys | (uint64_t)space->context << 48;
	adreno8_announce("secure translation 3a: prepare context %u with the handed-over tables;"
	    " configure stream %u as fault before enabling the context", SECURE_CONTEXT, SECURE_STREAM);
	/* Claim the route slot as fault first: the SMR write below then cannot expose a
	 * translate entry before the context holds our tables. */
	value = SMMU_S2CR_FAULT | SMMU_S2CR_SHCFG_NSH;
	if (!adreno8_3a_write(SMMU_S2CR(SECURE_MATCH), value, &read, "s2cr fault") ||
	    (read & 0x30000) != SMMU_S2CR_FAULT) {
		return adreno8_3a_stop("s2cr fault", value, read);
	}
	value = SMR_VALID | SMR_MASK << 16 | SECURE_STREAM;
	if (!adreno8_3a_write(SMMU_SMR(SECURE_MATCH), value, &read, "smr")) {
		return adreno8_3a_stop("smr", value, read);
	}
	/* Ownership probe: a bank this kernel may write reads back the zero just written. */
	value = 0;
	if (!adreno8_3a_write(cb + SMMU_CB_SCTLR, value, &read, "cb sctlr disabled")) {
		return adreno8_3a_stop("cb sctlr disabled", value, read);
	}
	/* CBAR selects stage 1 alone, CBA2R makes TTBR0 a 64-bit register (without it the
	 * high half of the root address need not take effect), MAIR0 supplies attribute
	 * index 0 which every PTE of this space uses. */
	value = STAGE_1;
	if (!adreno8_3a_write(adreno8_smmu_page + SMMU_CBAR(SECURE_CONTEXT), value, &read, "cb bar")) {
		return adreno8_3a_stop("cb bar", value, read);
	}
	value = 1;
	if (!adreno8_3a_write(adreno8_smmu_page + SMMU_CBA2R(SECURE_CONTEXT), value, &read, "cb a2r")) {
		return adreno8_3a_stop("cb a2r", value, read);
	}
	 /* TBI0/1 may read as one; the same allowance adreno8_space_on makes. */
	adreno8_put(&adreno8_smmu, "cb tcr2", cb + SMMU_CB_TCR2, TCR2_39_BITS);
	read = adreno8_get(&adreno8_smmu, "cb tcr2", cb + SMMU_CB_TCR2);
	printf("adreno8: %u: 3a cb tcr2 (%#x) write %#010x read %#010x\n", adreno8_step,
	    adreno8_smmu.phys + cb + SMMU_CB_TCR2, TCR2_39_BITS, read);
	if ((read & ~SMMU_TCR2_TBI) != TCR2_39_BITS) {
		return adreno8_3a_stop("cb tcr2", TCR2_39_BITS, read);
	}
	if (!adreno8_3a_write(cb + SMMU_CB_TCR, TCR_39_BITS, &read, "cb tcr")) {
		return adreno8_3a_stop("cb tcr", TCR_39_BITS, read);
	}
	if (!adreno8_3a_write(cb + SMMU_CB_TTBR0, (uint32_t)root, &read, "cb ttbr0, low")) {
		return adreno8_3a_stop("cb ttbr0, low", (uint32_t)root, read);
	}
	value = (uint32_t)(root >> 32);
	if (!adreno8_3a_write(cb + SMMU_CB_TTBR0 + 4, value, &read, "cb ttbr0, high")) {
		return adreno8_3a_stop("cb ttbr0, high", value, read);
	}
	if (!adreno8_3a_write(cb + SMMU_CB_MAIR0, MAIR0_NORMAL, &read, "cb mair0")) {
		return adreno8_3a_stop("cb mair0", MAIR0_NORMAL, read);
	}
	/* A stale fault bit would colour every later reading; this space owns no traffic yet. */
	adreno8_put(&adreno8_smmu, "cb fsr", cb + SMMU_CB_FSR, FSR_ALL);
	read = adreno8_get(&adreno8_smmu, "cb fsr", cb + SMMU_CB_FSR);
	printf("adreno8: %u: 3a cb fsr (%#x) write %#010x read %#010x\n", adreno8_step,
	    adreno8_smmu.phys + cb + SMMU_CB_FSR, FSR_ALL, read);
	/* FSR.Format [10:9] is read-only; 0x400 identifies AArch64 translation. */
	if ((read & FSR_ALL) != 0) {
		return adreno8_3a_stop("cb fsr", FSR_ALL, read);
	}
	adreno8_settle();
	adreno8_put(&adreno8_smmu, "cb tlb asid", cb + SMMU_CB_TLBASID, SECURE_CONTEXT);
	adreno8_put(&adreno8_smmu, "cb tlb sync", cb + SMMU_CB_TLBSYNC, 1);
	if (!adreno8_wait_for(&adreno8_smmu, cb + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		printf("adreno8: %u: secure translation 3a stopped at the TLB invalidate: context %u"
		    " TLBSTATUS did not clear\n", adreno8_step, SECURE_CONTEXT);
		return false;
	}
	adreno8_settle();
	/* Retain the context even if the enable write cannot be confirmed. */
	space->is_on = true;
	value = adreno8_smmu_sctlr(SECURE_CONTEXT);
	if (!adreno8_3a_write(cb + SMMU_CB_SCTLR, value, &read, "cb sctlr")) {
		return adreno8_3a_stop("cb sctlr", value, read);
	}
	adreno8_settle();
	read = adreno8_get(&adreno8_smmu, "cb fsr after enable", cb + SMMU_CB_FSR);
	printf("adreno8: %u: 3a cb fsr after enable (%#x) read %#010x\n", adreno8_step,
	    adreno8_smmu.phys + cb + SMMU_CB_FSR, read);
	if ((read & FSR_ALL) != 0) {
		return adreno8_3a_stop("cb fsr after enable", 0, read);
	}
	printf("adreno8: %u: secure translation 3a ready: context %u enabled, stream %u still"
	    " faulted, root PA %#llx\n", adreno8_step, SECURE_CONTEXT, SECURE_STREAM,
	    space->tables[0].phys);
	return true;
}

/* Mode 4 follows a successful 3a in the same boot. Only the route owned by
 * that setup changes. Failure retains the pages and requests a fault route. */
bool
adreno8_secure_translation_3b(struct adreno8_gpu_space *space)
{
	uint32_t cb = (adreno8_smmu_pages + SECURE_CONTEXT) * adreno8_smmu_page;
	uint32_t route = SMMU_S2CR_SHCFG_NSH | SECURE_CONTEXT;
	uint32_t blocked = SMMU_S2CR_FAULT | SMMU_S2CR_SHCFG_NSH;
	uint32_t sctlr = adreno8_smmu_sctlr(SECURE_CONTEXT);

	if (space != &adreno8_secure_space || !space->is_on || space->context != SECURE_CONTEXT) {
		return false;
	}
	if (adreno8_get(&adreno8_smmu, "secure smr", SMMU_SMR(SECURE_MATCH)) !=
	    (SMR_VALID | SMR_MASK << 16 | SECURE_STREAM) ||
	    (adreno8_get(&adreno8_smmu, "secure route before translate", SMMU_S2CR(SECURE_MATCH)) & 0x30303ff) != blocked ||
	    adreno8_get(&adreno8_smmu, "secure sctlr before translate", cb + SMMU_CB_SCTLR) != sctlr ||
	    adreno8_space_faulted(space)) {
		goto failed;
	}
	adreno8_put(&adreno8_smmu, "secure route translate", SMMU_S2CR(SECURE_MATCH), route);
	if ((adreno8_get(&adreno8_smmu, "secure route translate", SMMU_S2CR(SECURE_MATCH)) & 0x30303ff) != route) {
		goto failed;
	}
	adreno8_settle();
	if ((adreno8_get(&adreno8_smmu, "secure route settled", SMMU_S2CR(SECURE_MATCH)) & 0x30303ff) != route ||
	    adreno8_get(&adreno8_smmu, "secure sctlr after translate", cb + SMMU_CB_SCTLR) != sctlr ||
	    adreno8_space_faulted(space)) {
		goto failed;
	}
	printf("adreno8: secure translation 3b ready: stream %u match %u context %u route %#x;"
	    " SCTLR %#x, effective fault bits clear\n", SECURE_STREAM, SECURE_MATCH, SECURE_CONTEXT,
	    route, sctlr);
	return true;
failed:
	adreno8_put(&adreno8_smmu, "secure route fault", SMMU_S2CR(SECURE_MATCH), blocked);
	adreno8_settle();
	printf("adreno8: secure translation 3b failed; route readback %#x, pages retained until cold reset\n",
	    adreno8_get(&adreno8_smmu, "secure route after failure", SMMU_S2CR(SECURE_MATCH)));
	return false;
}
