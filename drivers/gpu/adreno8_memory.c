/* GPU/GMU shared allocations retained until a full device reset. */
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_secure_private.h"

#define printf adreno8_say
#define MEMORY_GRANULE 4096u
#define MEMORY_COUNT 64u
#define MEMORY_BLOCK_LIMIT (64u * 1024u * 1024u)
#define MEMORY_TOTAL_LIMIT (128u * 1024u * 1024u)
#define GPU_MATCH 0u
/* gfx3d_lpac SID 1, selected by CP_SMMU_STREAM_ID_LPAC=0x101. The BR SQE waits on
 * THREAD_SYNC for the LPAC engine, which fetches its microcode with this stream.
 * Linux msm puts SID 0 and 1 in one domain; this route shares GPU context 0. */
#define LPAC_STREAM 1u
#define LPAC_MATCH 3u

struct adreno8_memory_entry {
	struct adreno8_memory buffer;
	struct adreno8_hfi_mem_request request;
	bool from_hfi, complete, secure;
};

static struct adreno8_memory_entry adreno8_memory_entries[MEMORY_COUNT];
static uint32_t adreno8_memory_count;
static uint64_t adreno8_memory_bytes;
static bool adreno8_memory_failed;

bool
adreno8_memory_retained(void)
{
	/* Recorded pages and tables stay retained until cold reset. adreno8_secure_lost
	 * records uncertain ownership left by a failed or unconfirmed MP_ASSIGN; whenever
	 * it is set, the allocation and table counts above already hold retention. */
	return adreno8_memory_count != 0 || adreno8_gpu_space.table_count != 0 ||
	    adreno8_gpu_space.is_on || adreno8_secure_space.table_count != 0 ||
	    adreno8_secure_space.is_on || adreno8_secure_lost();
}

static struct adreno8_memory *
adreno8_memory_alloc(const char *name, uint32_t size, uint32_t alignment,
    uint32_t gpu_prot, uint32_t gmu_prot, bool gmu_cacheable)
{
	struct adreno8_memory_entry *entry;
	struct adreno8_memory *buffer;
	uint64_t allocated = ((uint64_t)size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
	uint32_t gpu_address = 0, gmu_address = 0;

	/* Only a fresh boot or validated mode-4 service can add normal buffers. */
	uint32_t state = adreno8_secure_xfer_state();
	if (state != SECURE_XFER_NOT_STARTED && state != SECURE_XFER_READY) {
		return NULL;
	}
	if (adreno8_memory_failed || adreno8_memory_count == MEMORY_COUNT || name == NULL ||
	    size == 0 || (size & (MEMORY_GRANULE - 1)) || size > MEMORY_BLOCK_LIMIT ||
	    alignment < MEMORY_GRANULE || (alignment & (alignment - 1)) ||
	    (gpu_prot && (!(gpu_prot & ADRENO8_MEM_READ) || (gpu_prot & ~15u))) ||
	    (gmu_prot && (!(gmu_prot & ADRENO8_MEM_READ) || (gmu_prot & ~15u))) ||
	    allocated > MEMORY_TOTAL_LIMIT - adreno8_memory_bytes) {
		return NULL;
	}
	if (gpu_prot && !adreno8_space_find_range(&adreno8_gpu_space, ADRENO8_GPU_IOVA_START,
	    ADRENO8_GPU_IOVA_END, size, alignment, &gpu_address)) {
		return NULL;
	}
	if (gmu_prot && (adreno8_gmu_space.table_count == 0 ||
	    !adreno8_space_find_range(&adreno8_gmu_space, gmu_cacheable ? 0x4000 : 0x60000000,
	    gmu_cacheable ? 0x1000000 : 0x80000000, size, alignment, &gmu_address))) {
		return NULL;
	}
	if (gpu_prot && adreno8_gpu_space.table_count == 0 &&
	    adreno8_space_new_table(&adreno8_gpu_space, 0, 0) == 0) {
		return NULL;
	}
	entry = &adreno8_memory_entries[adreno8_memory_count];
	buffer = &entry->buffer;
	if (kmem_alloc(kernel_map, &buffer->memory, (vm_size_t)allocated,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		buffer->memory = 0;
		return NULL;
	}
	/* Record ownership before any descriptor can expose this allocation to DMA. */
	adreno8_memory_count++;
	adreno8_memory_bytes += allocated;
	buffer->name = name;
	buffer->size = size;
	buffer->gpu_address = gpu_address;
	buffer->gmu_address = gmu_address;
	adreno8_to_memory(buffer->memory, (vm_size_t)allocated);
	if ((gpu_prot && !adreno8_space_map_region(&adreno8_gpu_space, gpu_address,
	    buffer->memory, size, gpu_prot)) ||
	    (gmu_prot && !adreno8_space_map_region(&adreno8_gmu_space, gmu_address,
	    buffer->memory, size, gmu_prot))) {
		adreno8_memory_failed = true;
		printf("adreno8: shared memory mapping failed; retain allocations until cold reset\n");
		return NULL;
	}
	entry->complete = true;
	printf("adreno8: shared buffer %s: size %#x, GPU %#x, GMU %#x\n",
	    name, size, gpu_address, gmu_address);
	return buffer;
}

/* Allocate one secure buffer and transfer ownership once. Modes 2/3 end without
 * a reply; mode 4 opens the service after context and route verification. All
 * pages remain allocated until cold reset, including every failed attempt. */
static struct adreno8_memory *
adreno8_memory_alloc_xfer(const char *name, uint32_t size, uint32_t alignment,
    uint32_t gpu_prot, uint64_t allocated, bool translate)
{
	struct adreno8_memory_entry *entry;
	struct adreno8_memory *buffer;
	uint32_t address = 0;

	if (adreno8_secure_xfer_state() != SECURE_XFER_NOT_STARTED) {
		printf("adreno8: the secure handover experiment already ended; this boot issues no further SCM call\n");
		return NULL;
	}
	if (adreno8_secure_space.table_count == 0 &&
	    adreno8_space_new_table(&adreno8_secure_space, 0, 0) == 0) {
		adreno8_secure_xfer_abort();
		return NULL;
	}
	if (!adreno8_space_find_range(&adreno8_secure_space, ADRENO8_SECURE_IOVA_START, ADRENO8_SECURE_IOVA_END,
	    size, alignment, &address)) {
		adreno8_secure_xfer_abort();
		return NULL;
	}
	entry = &adreno8_memory_entries[adreno8_memory_count];
	buffer = &entry->buffer;
	if (kmem_alloc(kernel_map, &buffer->memory, (vm_size_t)allocated,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		buffer->memory = 0;
		adreno8_secure_xfer_abort();
		return NULL;
	}
	/* Record ownership before any table can expose this allocation. */
	adreno8_memory_count++;
	adreno8_memory_bytes += allocated;
	buffer->name = name;
	buffer->size = size;
	buffer->gpu_address = address;
	entry->secure = true;
	/* Publish the zero fill while HLOS owns the pages; after the handover no
	 * cache line of theirs may be written back over what the secure owner stores. */
	adreno8_to_memory(buffer->memory, (vm_size_t)allocated);
	if (!adreno8_space_map_region(&adreno8_secure_space, address, buffer->memory,
	    size, gpu_prot)) {
		adreno8_memory_failed = true;
		printf("adreno8: secure allocation failed; retain GPU memory until cold reset\n");
		adreno8_secure_xfer_abort();
		return NULL;
	}
	/* Mode 3/4: prove the IOVA resolves through our own tables while the pages are
	 * still the kernel's; after the handover the CPU must not read them again. */
	if (translate && !adreno8_translation_walk(&adreno8_secure_space, buffer->gpu_address,
	    buffer->memory, size, gpu_prot)) {
		printf("adreno8: secure translation 3a stopped before the handover; no page transferred\n");
		adreno8_secure_xfer_abort();
		return NULL;
	}
	if (!adreno8_secure_xfer(&adreno8_secure_space, buffer)) {
		adreno8_memory_failed = true;
		printf("adreno8: secure allocation failed; retain GPU memory until cold reset\n");
		return NULL;
	}
	if (translate && !adreno8_secure_translation_3a(&adreno8_secure_space)) {
		entry->complete = adreno8_gpu8_secure_mode != SECURE_MODE_TRANSLATION_B;
		if (adreno8_gpu8_secure_mode == SECURE_MODE_TRANSLATION_B) {
			adreno8_memory_failed = true;
			adreno8_secure_xfer_abort();
		}
		printf("adreno8: secure translation 3a stopped after the handover; the pages stay assigned and no reply is built\n");
		return NULL;
	}
	if (adreno8_gpu8_secure_mode == SECURE_MODE_TRANSLATION_B) {
		if (!adreno8_secure_translation_3b(&adreno8_secure_space) || !adreno8_secure_xfer_ready()) {
			adreno8_memory_failed = true;
			adreno8_secure_xfer_abort();
			return NULL;
		}
		entry->complete = true;
		printf("adreno8: secure allocation ready for MEM_ALLOC: GPU %#x size %#x; pages retained until cold reset\n",
		    buffer->gpu_address, buffer->size);
		return buffer;
	}
	entry->complete = true;
	printf("adreno8: secure allocation experiment ends here: no MEM_ALLOC reply is built, START fails on purpose\n");
	return buffer;
}

/* kind 5 (CSW_PRIV_SECURE) buffers: the pages go to VMID CP_PIXEL through MP_ASSIGN.
 * Vendor review established the ordering invariant: the secure context attach,
 * its stream routing and the secure pagetable mapping are in place before the
 * firmware receives the kind-5 reply. Mode 0 keeps the path closed, mode 1
 * stops after the read-only preflight, mode 2 ends after MP_ASSIGN, mode 3
 * enables the context while its route stays faulted, and mode 4 validates the
 * translate route before replying. Secure pages never map into the default
 * GPU space; vendor kgsl_iommu.c keeps secure buffers in a separate secure
 * pagetable. The boot probe and the preflight only read registers; the kind-5
 * request itself can make the firmware touch secure buffers, which is why an
 * unverified path must not answer it. While CP_PIXEL owns the pages the secure
 * world refuses SID0 access; that refusal is a translation-time behavior only
 * a device run can observe, and host tests cover the driver side only. The CPU
 * must not read or write the pages after MP_ASSIGN, and no code path may treat
 * an SID0 access as a probe for recovered ownership. Page-table pages carry {HLOS read/write,
 * CP_PIXEL read} so the walker can still read tables the secure world owns.
 * The state of the secure bridge after the first handover is unknown: the
 * earlier park and PSCI verification covered runs without any MP_ASSIGN, and
 * whether a cold reset recovers a first handover still needs device
 * confirmation. A single MP_ASSIGN SMC may never return; the EBUSY retry bound
 * counts retries and a host-side timeout only bounds host waiting, it cancels
 * nothing inside TrustZone. */
static struct adreno8_memory *
adreno8_memory_alloc_secure(const char *name, uint32_t size, uint32_t alignment, uint32_t gpu_prot)
{
	uint64_t allocated = ((uint64_t)size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;

	if (adreno8_memory_failed || adreno8_memory_count == MEMORY_COUNT || name == NULL ||
	    size == 0 || (size & (MEMORY_GRANULE - 1)) || size > MEMORY_BLOCK_LIMIT ||
	    alignment < MEMORY_GRANULE || (alignment & (alignment - 1)) ||
	    !(gpu_prot & ADRENO8_MEM_READ) || (gpu_prot & ~15u) ||
	    allocated > MEMORY_TOTAL_LIMIT - adreno8_memory_bytes) {
		return NULL;
	}
	/* Read-only occupancy check of the candidate secure route, run before
	 * anything is staged for MP_ASSIGN; it writes no registers and issues
	 * no SCM call. A rejection is a stop before the first handover, so it
	 * leaves memory_failed alone and cannot mark pages uncertain. */
	if (adreno8_secure_xfer_state() != SECURE_XFER_NOT_STARTED || !adreno8_secure_preflight()) {
		return NULL;
	}
	switch (adreno8_gpu8_secure_mode) {
	case SECURE_MODE_HANDOVER:
		return adreno8_memory_alloc_xfer(name, size, alignment, gpu_prot, allocated, false);
	case SECURE_MODE_TRANSLATION_A:
	case SECURE_MODE_TRANSLATION_B:
		return adreno8_memory_alloc_xfer(name, size, alignment, gpu_prot, allocated, true);
	case SECURE_MODE_PREFLIGHT:
		printf("adreno8: secure preflight-only mode: START stops here without MP_ASSIGN or MEM_ALLOC reply\n");
		break;
	default:
		printf("adreno8: secure handover off: the MP_ASSIGN and MEM_ALLOC reply service awaits the secure translation implementation\n");
		break;
	}
	return NULL;
}

struct adreno8_memory *
adreno8_gpu_alloc(const char *name, uint32_t size, uint32_t alignment, uint32_t prot)
{
	uint64_t rounded = ((uint64_t)size + MEMORY_GRANULE - 1) & ~(uint64_t)(MEMORY_GRANULE - 1);

	if (size == 0 || rounded > MEMORY_BLOCK_LIMIT || prot == 0) {
		return NULL;
	}
	return adreno8_memory_alloc(name, (uint32_t)rounded, alignment, prot, 0, false);
}

struct adreno8_memory *
adreno8_gmu_alloc(const char *name, uint32_t size, uint32_t alignment, uint32_t prot)
{
	uint64_t rounded = ((uint64_t)size + MEMORY_GRANULE - 1) & ~(uint64_t)(MEMORY_GRANULE - 1);

	if (size == 0 || rounded > MEMORY_BLOCK_LIMIT || prot == 0) {
		return NULL;
	}
	return adreno8_memory_alloc(name, (uint32_t)rounded, alignment, 0, prot, false);
}

bool
adreno8_gpu_memory_ready(void)
{
	return adreno8_gpu_space.is_on && !adreno8_memory_failed;
}

bool
adreno8_gpu_memory_verify(void)
{
	if (adreno8_memory_failed) {
		return false;
	}
	if (!adreno8_cx_ready()) {
		adreno8_memory_failed = true;
		return false;
	}
	if (!adreno8_space_check(&adreno8_gpu_space, 0, GPU_MATCH) ||
	    !adreno8_space_route_check(&adreno8_gpu_space, LPAC_STREAM, LPAC_MATCH) || adreno8_gpu_faulted()) {
		adreno8_memory_failed = true;
		adreno8_hwsched_memory_failed();
		printf("adreno8: GPU memory verification failed; retain all memory and mappings until cold reset\n");
		return false;
	}
	return true;
}

bool
adreno8_gpu_memory_enable(void)
{
	if (adreno8_memory_failed || !adreno8_cx_ready()) {
		return false;
	}
	if (adreno8_gpu_space.is_on) {
		return adreno8_gpu_memory_verify();
	}
	if (!adreno8_space_on(&adreno8_gpu_space, 0, GPU_MATCH) ||
	    !adreno8_space_route(&adreno8_gpu_space, LPAC_STREAM, LPAC_MATCH) || !adreno8_scm_gpu_aperture()) {
		adreno8_memory_failed = true;
		return false;
	}
	return adreno8_gpu_memory_verify();
}

bool
adreno8_gpu_faulted(void)
{
	bool fault = adreno8_memory_failed;
	if (adreno8_gpu_space.is_on) fault |= adreno8_space_faulted(&adreno8_gpu_space);
	if (adreno8_gmu_space.is_on) fault |= adreno8_space_faulted(&adreno8_gmu_space);
	if (adreno8_secure_space.is_on) fault |= adreno8_space_faulted(&adreno8_secure_space);
	return fault;
}

static struct adreno8_memory_entry *
adreno8_memory_entry(const struct adreno8_memory *buffer)
{
	for (unsigned i = 0; i < adreno8_memory_count; i++) {
		if (buffer == &adreno8_memory_entries[i].buffer && adreno8_memory_entries[i].complete) {
			return &adreno8_memory_entries[i];
		}
	}
	return NULL;
}

/* Secure buffers have no CPU access left after MP_ASSIGN; publish and acquire
 * would read or write pages the secure world owns. */
static bool
adreno8_memory_range(const struct adreno8_memory *buffer, uint32_t offset, uint32_t size)
{
	struct adreno8_memory_entry *entry = adreno8_memory_entry(buffer);

	return entry != NULL && !entry->secure && size != 0 && offset <= buffer->size &&
	    size <= buffer->size - offset;
}

bool
adreno8_memory_publish(const struct adreno8_memory *buffer, uint32_t offset, uint32_t size)
{
	if (!adreno8_memory_range(buffer, offset, size)) return false;
	adreno8_to_memory(buffer->memory + offset, size);
	return true;
}

static void
adreno8_memory_invalidate(vm_offset_t start, uint32_t size)
{
	adreno8_settle();
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc ivac, %0" : : "r" (line) : "memory");
	}
	adreno8_settle();
}

bool
adreno8_memory_acquire(const struct adreno8_memory *buffer, uint32_t offset, uint32_t size)
{
	if (!adreno8_memory_range(buffer, offset, size)) return false;
	adreno8_memory_invalidate(buffer->memory + offset, size);
	return true;
}

bool
adreno8_hwsched_alloc(const struct adreno8_hfi_mem_request *request, struct adreno8_hfi_mem_result *result)
{
	uint32_t gpu_prot = 0, gmu_prot = 0, va_shift, size_shift;
	uint64_t rounded;
	struct adreno8_memory_entry *entry = NULL;
	struct adreno8_memory *buffer = NULL;
	bool secure;

	if (result == NULL) return false;
	memset(result, 0, sizeof(*result));
	if (request == NULL) return false;
	uint32_t state = adreno8_secure_xfer_state();
	if (state != SECURE_XFER_NOT_STARTED && state != SECURE_XFER_READY) {
		printf("adreno8: the secure handover experiment ended; the memory path stops here\n");
		return false;
	}
	if (state == SECURE_XFER_READY && adreno8_gpu_faulted()) {
		adreno8_hwsched_memory_failed();
		return false;
	}
	/* Only the exact CSW_PRIV_SECURE shape earns a secure owner: kind 5 with the
	 * vendor's GFX|GFX_PRIV|GFX_WRITE|GFX_SECURE flags. The non-secure flag mask
	 * keeps rejecting bit 12 for every other kind and flag combination. */
	secure = request->kind == 5 && request->flags == 0x1007u;
	if (adreno8_memory_failed || request->size == 0 || request->kind >= 27 ||
	    (request->align & ~0xffffu) || (!secure && (request->flags & ~0x3ffu))) {
		return false;
	}
	/* These kinds require secure or peripheral resources that this allocator does not own. */
	if ((request->kind == 5 && !secure) || request->kind == 11 || request->kind == 12 ||
	    request->kind == 13 || request->kind == 19 || request->kind == 20 ||
	    request->kind == 23) return false;
	for (unsigned i = 0; i < adreno8_memory_count; i++) {
		struct adreno8_memory_entry *candidate = &adreno8_memory_entries[i];
		if (candidate->from_hfi && candidate->request.kind == request->kind &&
		    candidate->request.handle == request->handle) {
			if (!candidate->complete || candidate->request.flags != request->flags ||
			    candidate->request.size != request->size || candidate->request.align != request->align) return false;
			entry = candidate;
			buffer = &candidate->buffer;
			goto success;
		}
	}
	va_shift = request->align & 0xff;
	size_shift = request->align >> 8;
	if (va_shift > 31 || size_shift > 31) return false;
	if (va_shift < 12) va_shift = 12;
	if (size_shift < 12) size_shift = 12;
	rounded = ((uint64_t)request->size + (1ULL << size_shift) - 1) & ~((1ULL << size_shift) - 1);
	if (rounded > MEMORY_BLOCK_LIMIT) return false;
	if (request->flags & ADRENO8_HFI_MEM_GFX) {
		gpu_prot = ADRENO8_MEM_READ;
		if (request->flags & ADRENO8_HFI_MEM_GFX_WRITE) gpu_prot |= ADRENO8_MEM_WRITE;
		if (request->flags & ADRENO8_HFI_MEM_GFX_PRIV) gpu_prot |= ADRENO8_MEM_PRIV;
	}
	if (!(request->flags & ADRENO8_HFI_MEM_GFX)) {
		/* The vendor's GMU-only reserve path always grants privileged read/write access. */
		gmu_prot = ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV | ADRENO8_MEM_EXEC;
	} else if (request->flags & ADRENO8_HFI_MEM_GMU) {
		/* Vendor get_attrs leaves GMU execute permission enabled; CACHEABLE selects its VA range. */
		gmu_prot = ADRENO8_MEM_READ | ADRENO8_MEM_EXEC;
		if (request->flags & ADRENO8_HFI_MEM_GMU_WRITE) gmu_prot |= ADRENO8_MEM_WRITE;
		if (request->flags & ADRENO8_HFI_MEM_GMU_PRIV) gmu_prot |= ADRENO8_MEM_PRIV;
	}
	if (secure) {
		buffer = adreno8_memory_alloc_secure("hwsched-secure", (uint32_t)rounded,
		    1u << va_shift, gpu_prot);
	} else {
		buffer = adreno8_memory_alloc("hwsched", (uint32_t)rounded, 1u << va_shift,
		    gpu_prot, gmu_prot, (request->flags & ADRENO8_HFI_MEM_CACHEABLE) != 0);
	}
	if (buffer == NULL) return false;
	entry = &adreno8_memory_entries[adreno8_memory_count - 1];
	entry->from_hfi = true;
	entry->request = *request;
	if (secure && adreno8_secure_xfer_state() != SECURE_XFER_READY) {
		return false;
	}
success:
	result->gpu_address = buffer->gpu_address;
	result->gmu_address = buffer->gmu_address;
	result->size = buffer->size;
	result->secure = entry->secure;
	return true;
}

/* HFI owns the failure notification; this module owns allocation admission.
 * No failure returns assigned memory to HLOS or makes another transfer legal. */
void
adreno8_hwsched_memory_failed(void)
{
	if (adreno8_gpu8_secure_mode != SECURE_MODE_TRANSLATION_B) {
		return;
	}
	adreno8_secure_xfer_abort();
	adreno8_memory_failed = true;
	if (adreno8_secure_space.is_on) {
		(void)adreno8_space_faulted(&adreno8_secure_space);
	}
}
