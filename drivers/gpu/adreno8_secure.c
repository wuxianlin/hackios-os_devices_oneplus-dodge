/* MP_ASSIGN ownership transfer backing the HFI kind-5 secure allocation. */
#include "adreno8_secure_private.h"

#define printf adreno8_say

/* QCOM_SCM_SVC_MP (0x0c) + QCOM_SCM_MP_ASSIGN (0x16) in the SMC64 standard call
 * encoding; the arginfo matches the vendor's seven-argument type list
 * (RO, VAL, RO, VAL, RO, VAL, VAL). Arguments 3..6 travel in the extended
 * buffer whose physical address occupies register slot x5. */
#define SCM_MP_ASSIGN        0x42000c16ull
#define SCM_ASSIGN_ARGINFO   0x1117ull
/* vendor BATCH_MAX_SECTIONS: region entries per SCM call. The vendor caps a
 * batch at BATCH_MAX_SIZE (2 MiB) on top (vendor-secure_buffer.c); this
 * driver batches by entry count only. The current kind-5 request (0x138000)
 * stays under 2 MiB; the vendor's reason for that cap is unverified. */
#define ASSIGN_SECTIONS      32u
#define ASSIGN_EBUSY         (-12)
/* 20 attempts, 30 ms sleep after each EBUSY (20 sleeps, including one after
 * the last EBUSY). The vendor loop allows up to 22 attempts and 21 sleeps
 * (vendor-qcom_scm-smc.c); the counts differ. */
#define ASSIGN_EBUSY_RETRY   20u
#define ASSIGN_EBUSY_MS      30u
#define VMID_HLOS            3u
#define VMID_CP_PIXEL        0xAu
#define PERM_READ            4u
#define PERM_WRITE           2u
#define PERM_EXEC            1u

/* Little-endian wire records; this kernel is arm64-only, so plain stores match
 * the vendor's cpu_to_le64 layout exactly. */
struct assign_region {
	uint64_t address, size;
};
struct assign_perm {
	uint32_t vmid, perm;
	uint64_t context;
	uint32_t context_size, unused;
};

#define ASSIGN_SOURCES      512u       /* after 32 region slots */
#define ASSIGN_PERMS        576u       /* 64-byte aligned, room for two entries */
#define ASSIGN_EXTENDED     640u       /* four little-endian argument words */
#define ASSIGN_USED         (ASSIGN_EXTENDED + 4 * sizeof(uint64_t))

/* Record sizes come from the vendor wire format; the 512/576/640 offsets are
 * this driver's staging layout inside the argument page.  Every used byte
 * must stay inside one page. */
_Static_assert(sizeof(struct assign_region) == 16, "MP_ASSIGN region record must be 16 bytes");
_Static_assert(sizeof(struct assign_perm) == 24, "MP_ASSIGN permission record must be 24 bytes");
/* XNU ARM PAGE_MIN_SHIFT is 12. PAGE_SIZE can be runtime-valued in
 * exported arm64 headers; the layout must fit the smallest supported page. */
_Static_assert(ASSIGN_USED <= 4096u,
    "MP_ASSIGN argument layout must fit the smallest supported kernel page");

static struct {
	vm_offset_t page;
	uint64_t page_phys;
	bool lost;
	unsigned attempts;               /* top-level SCM calls actually made, EBUSY
	                                  * retries included; the A6 interrupted
	                                  * continuation SMCs stay hidden */
} adreno8_assign;

bool
adreno8_secure_lost(void)
{
	return adreno8_assign.lost;
}

/* Prove one kernel page is wholly ours and mappable: a virtual address with
 * this kernel's native alignment, a real physical start aligned to the SMMU
 * granule and to this kernel's page size, every 4 KiB granule of the page
 * mapping at first + offset (a 16 KiB kernel page may sit on four separate
 * 4 KiB SMMU leaves, and a middle leaf can move while both ends still look
 * right), and ending inside the range the SMMU can translate. Every page
 * that reaches MP_ASSIGN passes through here, including the argument page. */
static bool
adreno8_assign_page_phys(vm_offset_t va, uint64_t *phys)
{
	uint64_t first = kvtophys(va);

	*phys = first;
	if ((va & (PAGE_SIZE - 1)) || first == 0 || (first & SMMU_GRANULE_MASK) ||
	    (first & (PAGE_SIZE - 1)) || first > SMMU_PHYS_LIMIT - PAGE_SIZE) {
		return false;
	}
	/* Walk every granule boundary instead of trusting the two ends: the
	 * range test subtracts PAGE_SIZE so no uint64 sum can wrap around. */
	for (vm_offset_t at = 0; at < PAGE_SIZE; at += SMMU_GRANULE) {
		if (kvtophys(va + at) != first + at) {
			return false;
		}
	}
	return kvtophys(va + PAGE_SIZE - 1) == first + PAGE_SIZE - 1;
}

/* One physically contiguous, zeroed argument page. A page that fails
 * verification here is freed and does not mark `lost`: the argument page is a
 * plain HLOS page never handed to the secure world, so its ownership is never
 * in doubt. The failed handover itself is terminal — adreno8_secure_xfer_abort
 * closes the state and no call retries it this boot; the free only returns a
 * page the secure world never saw. */
static bool
adreno8_assign_open(void)
{
	uint64_t first;

	if (adreno8_assign.lost) {
		return false;
	}
	if (adreno8_assign.page != 0) {
		return true;
	}
	if (kmem_alloc(kernel_map, &adreno8_assign.page, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		adreno8_assign.page = 0;
		return false;
	}
	if (!adreno8_assign_page_phys(adreno8_assign.page, &first)) {
		kmem_free(kernel_map, adreno8_assign.page, PAGE_SIZE);
		adreno8_assign.page = 0;
		return false;
	}
	adreno8_assign.page_phys = first;
	printf("adreno8: MP_ASSIGN argument page at %#llx\n", first);
	return true;
}

static int32_t
adreno8_assign_call(uint64_t regions_bytes, uint64_t perms_bytes, uint64_t *answer_out)
{
	uint32_t *sources = (uint32_t *)(adreno8_assign.page + ASSIGN_SOURCES);
	uint64_t *extended = (uint64_t *)(adreno8_assign.page + ASSIGN_EXTENDED);
	uint64_t sources_phys = adreno8_assign.page_phys + ASSIGN_SOURCES;
	uint64_t perms_phys = adreno8_assign.page_phys + ASSIGN_PERMS;
	uint64_t extended_phys = adreno8_assign.page_phys + ASSIGN_EXTENDED;
	uint64_t answer = 1;
	int32_t status = ASSIGN_EBUSY;

	sources[0] = VMID_HLOS;
	extended[0] = sizeof(uint32_t);
	extended[1] = perms_phys;
	extended[2] = perms_bytes;
	extended[3] = 0;
	/* TZ reads every staged record from DRAM: clean after the last store. */
	adreno8_to_memory(adreno8_assign.page, ASSIGN_USED);
	/*
	 * The retry bound counts EBUSY retries; it is not a wall-clock bound.
	 * A single SMC may not return at all, and then no host-side timeout
	 * cancels the TrustZone call; a host timeout only bounds host waiting.
	 */
	for (unsigned retry = 0; retry < ASSIGN_EBUSY_RETRY; retry++) {
		adreno8_assign.attempts++;
		status = adreno8_scm_call(SCM_MP_ASSIGN, SCM_ASSIGN_ARGINFO, adreno8_assign.page_phys,
		    regions_bytes, sources_phys, extended_phys, &answer);
		if (status != ASSIGN_EBUSY) {
			break;
		}
		adreno8_sleep_ms(ASSIGN_EBUSY_MS);
	}
	*answer_out = answer;
	return status;
}

/* The caller has staged `count` region entries in @regions. A failed batch can
 * have been applied to TZ partially and the HLOS cannot tell which regions
 * landed, so every page of this operation stays assigned where it is until
 * cold reset; nothing here rolls back or frees. `role`, `perm_note` and `batch` label
 * the lines printed from the staged batch itself, so a table-batch failure
 * reads apart from a data-batch one and the log matches what the SMC gets. */
static bool
adreno8_assign_flush(const struct assign_region *regions, unsigned count,
    const struct assign_perm *perms, unsigned perm_count, const char *role,
    const char *perm_note, unsigned batch)
{
	struct assign_region *slots = (struct assign_region *)adreno8_assign.page;
	struct assign_perm *perm_slots = (struct assign_perm *)(adreno8_assign.page + ASSIGN_PERMS);
	int32_t status;
	uint64_t answer;

	for (unsigned i = 0; i < count; i++) {
		slots[i] = regions[i];
	}
	for (unsigned i = 0; i < perm_count; i++) {
		perm_slots[i] = perms[i];
	}
	printf("adreno8: MP_ASSIGN plan: batch %u %s, permissions %s, argument page %#llx, %u regions\n",
	    batch, role, perm_note, (unsigned long long)adreno8_assign.page_phys, count);
	for (unsigned i = 0; i < count; i++) {
		printf("adreno8: MP_ASSIGN plan: batch %u region %u addr %#llx len %llu\n",
		    batch, i, (unsigned long long)regions[i].address,
		    (unsigned long long)regions[i].size);
	}
	printf("adreno8: MP_ASSIGN batch %u %s: %u regions, %u permission records, %u top-level SCM calls before\n",
	    batch, role, count, perm_count, adreno8_assign.attempts);
	status = adreno8_assign_call((uint64_t)count * sizeof(struct assign_region),
	    (uint64_t)perm_count * sizeof(struct assign_perm), &answer);
	printf("adreno8: MP_ASSIGN batch %u %s returned %d, answer %#llx, %u top-level SCM calls (A6 interrupted continuation SMCs not counted)\n",
	    batch, role, status, (unsigned long long)answer, adreno8_assign.attempts);
	if (status == 0 && answer == 0) {
		return true;
	}
	adreno8_assign.lost = true;
	printf("adreno8: secure pages retained until cold reset\n");
	return false;
}

/* Stage and flush the buffer's data pages to CP_PIXEL read/write. The caller has
 * passed the preflight and opened the argument page; `batch_base` is the count
 * of table batches, so table and data batch numbers continue one sequence. */
static bool
adreno8_assign_data(const struct adreno8_memory *buffer, unsigned batch_base)
{
	struct assign_perm perms[] = { { VMID_CP_PIXEL, PERM_READ | PERM_WRITE, 0, 0, 0 } };
	struct assign_region batch_regions[ASSIGN_SECTIONS];
	uint64_t bytes = ((uint64_t)buffer->size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
	unsigned filled = 0, batch = batch_base;

	for (uint64_t at = 0; at < bytes; ) {
		uint64_t phys, next;

		if (!adreno8_assign_page_phys(buffer->memory + at, &phys)) {
			/*
			 * A page whose physical start cannot be proven closes the secure path for
			 * the whole boot: the driver cannot learn which regions TZ accepted, so
			 * every page of this buffer stays reserved until cold reset. Reaching it
			 * means at least one MP_ASSIGN already went out: adreno8_assign_plan
			 * proves every data page before the first batch, and the table batches
			 * precede the data batches. The check reads only page tables through
			 * kvtophys, so no CPU read probes a transferred page for recovery; a
			 * cold reset recovering a first handover is itself unconfirmed and needs
			 * device-side confirmation.
			 */
			printf("adreno8: secure page %#llx unusable for MP_ASSIGN\n", phys);
			adreno8_assign.lost = true;
			printf("adreno8: secure pages retained until cold reset\n");
			return false;
		}
		uint64_t run = PAGE_SIZE;
		while (at + run < bytes &&
		    adreno8_assign_page_phys(buffer->memory + at + run, &next) && next == phys + run) {
			run += PAGE_SIZE;
		}
		batch_regions[filled++] = (struct assign_region){ phys, run };
		at += run;
		if (filled == ASSIGN_SECTIONS) {
			if (!adreno8_assign_flush(batch_regions, filled, perms, 1, "data",
			    "CP_PIXEL read/write", batch++)) {
				return false;
			}
			filled = 0;
		}
	}
	if (filled != 0 && !adreno8_assign_flush(batch_regions, filled, perms, 1, "data",
	    "CP_PIXEL read/write", batch)) {
		return false;
	}
	printf("adreno8: secure buffer %s: %llu KiB owned by CP_PIXEL\n",
	    buffer->name, (unsigned long long)(bytes / 1024));
	return true;
}

/* Check every page-table page past the cursor before the first MP_ASSIGN:
 * whole-page physical provenance per adreno8_assign_page_phys and agreement
 * with the address the mapper recorded. A pass lets adreno8_assign_tables
 * stage and flush without reaching the retention path. */
static bool
adreno8_assign_tables_check(const struct adreno8_gpu_space *space)
{
	for (unsigned i = space->secure_tables; i < space->table_count; i++) {
		uint64_t phys = space->tables[i].table;

		if (space->tables[i].table == 0 ||
		    !adreno8_assign_page_phys(space->tables[i].table, &phys) ||
		    phys != space->tables[i].phys) {
			printf("adreno8: secure table page %#llx unusable for MP_ASSIGN\n", phys);
			return false;
		}
	}
	return true;
}

/* Assign page-table pages past the cursor to {HLOS read/write, CP_PIXEL read}.
 * The caller ran adreno8_assign_tables_check and opened the argument page;
 * committing the cursor is the caller's. */
static bool
adreno8_assign_tables(struct adreno8_gpu_space *space)
{
	struct assign_perm perms[] = {
		{ VMID_HLOS, PERM_READ | PERM_WRITE, 0, 0, 0 },
		{ VMID_CP_PIXEL, PERM_READ, 0, 0, 0 },
	};
	struct assign_region batch_regions[ASSIGN_SECTIONS];
	unsigned filled = 0, batch = 0;

	for (unsigned i = space->secure_tables; i < space->table_count; i++) {
		uint64_t phys;

		if (!adreno8_assign_page_phys(space->tables[i].table, &phys) ||
		    phys != space->tables[i].phys) {
			/* Same conservative rule as adreno8_assign_data: an unproven page ends
			 * the secure path for the boot and every table page stays reserved. */
			adreno8_assign.lost = true;
			printf("adreno8: secure pages retained until cold reset\n");
			return false;
		}
		/* Clean the whole kernel page: the SMMU walker only consumes the first
		 * 4 KiB, but MP_ASSIGN transfers the native page, which is 16 KiB when
		 * XNU runs with 16 KiB pages. */
		adreno8_to_memory(space->tables[i].table, PAGE_SIZE);
		batch_regions[filled++] = (struct assign_region){ phys, PAGE_SIZE };
		if (filled == ASSIGN_SECTIONS) {
			if (!adreno8_assign_flush(batch_regions, filled, perms, 2, "table",
			    "HLOS read/write and CP_PIXEL read", batch++)) {
				return false;
			}
			filled = 0;
		}
	}
	if (filled != 0 && !adreno8_assign_flush(batch_regions, filled, perms, 2, "table",
	    "HLOS read/write and CP_PIXEL read", batch)) {
		return false;
	}
	return true;
}

/* Count the data pages and merge runs into region entries without staging, so
 * an experiment can print its batch plan before any MP_ASSIGN. The counts come
 * from the real PAGE_SIZE and the merge rule adreno8_assign_data uses. */
static bool
adreno8_assign_plan(const struct adreno8_memory *buffer, unsigned *pages, unsigned *regions)
{
	uint64_t bytes = ((uint64_t)buffer->size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;

	*pages = 0;
	*regions = 0;
	for (uint64_t at = 0; at < bytes; ) {
		uint64_t phys, next;

		if (!adreno8_assign_page_phys(buffer->memory + at, &phys)) {
			printf("adreno8: secure page %#llx unusable for MP_ASSIGN\n", phys);
			return false;
		}
		uint64_t run = PAGE_SIZE;
		while (at + run < bytes &&
		    adreno8_assign_page_phys(buffer->memory + at + run, &next) && next == phys + run) {
			run += PAGE_SIZE;
		}
		*pages += (unsigned)(run / PAGE_SIZE);
		*regions += 1;
		at += run;
	}
	return true;
}

/* One ownership transfer per boot; only validated mode 4 opens the service. */
static uint32_t adreno8_xfer_state;

uint32_t
adreno8_secure_xfer_state(void)
{
	return adreno8_xfer_state;
}

/* Close an unstarted transfer or the validated service, preserving ownership. */
void
adreno8_secure_xfer_abort(void)
{
	if (adreno8_xfer_state == SECURE_XFER_ASSIGNED || adreno8_xfer_state == SECURE_XFER_READY) {
		adreno8_xfer_state = SECURE_XFER_TERMINAL;
		printf("adreno8: secure service closed; all pages retained until cold reset\n");
		return;
	}
	if (adreno8_xfer_state != SECURE_XFER_NOT_STARTED) {
		return;
	}
	adreno8_xfer_state = SECURE_XFER_TERMINAL;
	printf("adreno8: secure handover ends before any MP_ASSIGN; no page transferred\n");
}

bool
adreno8_secure_xfer_ready(void)
{
	if (adreno8_gpu8_secure_mode != SECURE_MODE_TRANSLATION_B ||
	    adreno8_xfer_state != SECURE_XFER_ASSIGNED || adreno8_assign.lost ||
	    !adreno8_secure_space.is_on || adreno8_secure_space.table_count == 0 ||
	    adreno8_secure_space.secure_tables != adreno8_secure_space.table_count) {
		return false;
	}
	adreno8_xfer_state = SECURE_XFER_READY;
	return true;
}

/*
 * One isolated handover of this kernel's own kind-5 buffer: assign the secure
 * page-table pages, then the data pages, and stop. The caller runs
 * adreno8_secure_preflight first; this function is reached only from that one
 * kernel call site. No MEM_ALLOC reply, no CB3 enable, no CB0 mapping and no
 * address reaches the firmware in this function. A successful mode-4 transfer
 * ends as ASSIGNED; other outcomes end as TERMINAL. The pages stay retained;
 * a success keeps them with the secure owner
 * rather than marking them lost. This does not prove the secure bridge can
 * hand memory back, a path the earlier park and PSCI checks never exercised.
 */
bool
adreno8_secure_xfer(struct adreno8_gpu_space *space, const struct adreno8_memory *buffer)
{
	unsigned pages, regions, batches, table_batches;

	if (adreno8_xfer_state != SECURE_XFER_NOT_STARTED) {
		return false;
	}
	/* The rules this path can prove from its arguments alone: the buffer
	 * must exist, the secure context must still be disabled for the
	 * experiment, and the table list must be non-empty and inside the
	 * array the space carries, with the cursor at or below the count (a
	 * cursor equal to the count contributes no table batches). buffer and
	 * space are caller-owned structures that the allocation path
	 * dereferences before calling, so neither may be NULL; table contents
	 * are proven page by page in adreno8_assign_tables_check. buffer->size
	 * is a uint32, so rounding it up in uint64 cannot overflow. */
	if (buffer->memory == 0 || buffer->size == 0 || space->is_on ||
	    space->table_count == 0 ||
	    space->table_count > sizeof(space->tables) / sizeof(space->tables[0]) ||
	    space->secure_tables > space->table_count) {
		printf("adreno8: secure handover input rejected: buffer %#llx size %u, space %u tables cursor %u on %d\n",
		    (uint64_t)buffer->memory, buffer->size, space->table_count,
		    space->secure_tables, space->is_on);
		return false;
	}
	if (!adreno8_assign_plan(buffer, &pages, &regions) ||
	    !adreno8_assign_tables_check(space) || !adreno8_assign_open()) {
		adreno8_secure_xfer_abort();
		return false;
	}
	adreno8_xfer_state = SECURE_XFER_ATTEMPTED;
	table_batches = (space->table_count - space->secure_tables + ASSIGN_SECTIONS - 1) /
	    ASSIGN_SECTIONS;
	batches = (regions + ASSIGN_SECTIONS - 1) / ASSIGN_SECTIONS + table_batches;
	printf("adreno8: secure handover attempted: %u pages merge into %u data regions plus %u table pages, %u logical MP_ASSIGN batches\n",
	    pages, regions, space->table_count - space->secure_tables, batches);
	if (!adreno8_assign_tables(space) || !adreno8_assign_data(buffer, table_batches)) {
		adreno8_xfer_state = SECURE_XFER_TERMINAL;
		return false;
	}
	space->secure_tables = space->table_count;
	adreno8_xfer_state = adreno8_gpu8_secure_mode == SECURE_MODE_TRANSLATION_B ?
	    SECURE_XFER_ASSIGNED : SECURE_XFER_TERMINAL;
	printf("adreno8: secure handover complete: %u pages owned by CP_PIXEL after %u top-level MP_ASSIGN SCM calls, A6 interrupted continuation SMCs not counted, retained until cold reset; no MEM_ALLOC reply from the ownership-transfer step\n",
	    pages, adreno8_assign.attempts);
	return true;
}
