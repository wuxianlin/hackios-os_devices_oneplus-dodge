/* gen8.0.0 HWSCHED startup, using the vendor cold-boot HFI sequence. */
#include "adreno8_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_clock_private.h"

#define printf adreno8_say

static bool adreno8_hwsched_attempted, adreno8_hwsched_started;

static bool
adreno8_hwsched_mem_request(const uint32_t *msg, uint32_t words,
    uint32_t *reply, uint32_t *reply_words)
{
	struct adreno8_hfi_mem_request request;
	struct adreno8_hfi_mem_result result = {0};
	uint32_t va_bits, size_bits;
	uint64_t va_alignment, size_alignment, mapped_size;
	bool secure;

	/* Version 0 omits align; version 1 adds its two alignment exponents. */
	if (words < 2 || !((msg[1] == 0 && words == 10) || (msg[1] == 1 && words == 11))) {
		return false;
	}
	if (msg[2] != 0 || msg[3] != 0 || msg[6] != 0 || msg[8] != 0) {
		printf("adreno8: MEM_ALLOC supplied an address or host handle\n");
		return false;
	}
	request.flags = msg[4];
	request.kind = msg[5];
	request.handle = msg[7];
	request.size = msg[9];
	request.align = words == 11 ? msg[10] : 0;
	if ((request.flags & ~0x13ffu) != 0 || request.size == 0 || request.kind >= 27 ||
	    (request.align & 0xffff0000u) != 0) {
		return false;
	}
	printf("adreno8: MEM_ALLOC kind %u handle %#x flags %#x size %#x align %#x\n",
	    request.kind, request.handle, request.flags, request.size, request.align);
	/* gen80000 requests this GPU-only secure preemption record even with
	 * PREEMPTION disabled. Its allocation must be completed by the secure owner. */
	secure = request.kind == 5 && request.flags == 0x1007;
	if ((((request.flags & ADRENO8_HFI_MEM_SECURE) || request.kind == 5) && !secure) ||
	    request.kind == 11 || request.kind == 12 || request.kind == 13 ||
	    request.kind == 19 || request.kind == 20 || request.kind == 23) {
		printf("adreno8: unsupported MEM_ALLOC kind %u flags %#x\n", request.kind, request.flags);
		return false;
	}
	va_bits = request.align & 0xff;
	size_bits = (request.align >> 8) & 0xff;
	if (va_bits > 31 || size_bits > 31) {
		return false;
	}
	va_alignment = 1ULL << (va_bits < 12 ? 12 : va_bits);
	size_alignment = 1ULL << (size_bits < 12 ? 12 : size_bits);
	mapped_size = ((uint64_t)request.size + size_alignment - 1) & ~(size_alignment - 1);
	if (mapped_size > 0xffffffffULL) {
		return false;
	}
	if (!adreno8_hwsched_alloc(&request, &result)) {
		return false;
	}
	if (result.secure != secure || result.size < mapped_size ||
	    (result.size & (size_alignment - 1)) != 0) {
		return false;
	}
	/* Vendor GMU-only allocations always receive a GMU VA. GMU_ACC controls
	 * the additional GMU mapping only for allocations shared with the GPU. */
	if ((request.flags & ADRENO8_HFI_MEM_GMU) || !(request.flags & ADRENO8_HFI_MEM_GFX)) {
		if (result.gmu_address == 0 || (result.gmu_address & (va_alignment - 1)) != 0 ||
		    (uint64_t)result.gmu_address + result.size > 0x100000000ULL) {
			return false;
		}
	} else if (result.gmu_address != 0) {
		return false;
	}
	if (request.flags & ADRENO8_HFI_MEM_GFX) {
		if (result.gpu_address == 0 || (result.gpu_address & 0xfff) != 0 ||
		    result.gpu_address > ~0ULL - result.size) {
			return false;
		}
	} else if (result.gpu_address != 0) {
		return false;
	}
	reply[0] = msg[0];
	reply[1] = (uint32_t)result.gpu_address;
	reply[2] = (uint32_t)(result.gpu_address >> 32);
	reply[3] = request.flags;
	reply[4] = request.kind;
	reply[5] = request.handle;
	reply[6] = request.handle;
	reply[7] = result.gmu_address;
	/* The vendor replies with the requested size, even if its allocation is larger. */
	reply[8] = request.size;
	reply[9] = request.align;
	*reply_words = 10;
	return true;
}

bool
adreno8_hwsched_request(const uint32_t *request, uint32_t words,
    uint32_t *reply, uint32_t *reply_words)
{
	if (request == NULL || reply == NULL || reply_words == NULL || words == 0 ||
	    words != ((request[0] >> 8) & 0xff) || ((request[0] >> 16) & 0xf) != 0) {
		return false;
	}
	*reply_words = 0;
	switch (request[0] & 0xff) {
	case 20:
		return adreno8_hwsched_mem_request(request, words, reply, reply_words);
	case 110:
		if (words != 3) {
			return false;
		}
		/* The vendor permits unavailable counters: zero register offsets report
		 * allocation failure without accessing GX registers during START. */
		reply[0] = request[0];
		reply[1] = request[1];
		reply[2] = request[2];
		reply[3] = reply[4] = 0;
		*reply_words = 5;
		return true;
	default:
		return false;
	}
}

bool
adreno8_hwsched_ready(void)
{
	return !adreno8_direct_ring && adreno8_hwsched_started &&
	    (adreno8_gpu8_secure_mode != SECURE_MODE_TRANSLATION_B || !adreno8_gpu_faulted());
}

bool
adreno8_hwsched_start(void)
{
	const uint32_t features[][3] = {{9, 0, 0}, {1, 1, 0}, {16, 1, 0}};
	const uint32_t handle = 0, preemption[] = {2, 0, 0}, aqe[] = {29, 1, 0};

	if (adreno8_direct_ring || adreno8_hwsched_attempted || !adreno8_cx_ready() || !adreno8_gmu_talks ||
	    !adreno8_hfi_tables_sent || !adreno8_gpu8_keep || adreno8_gpu8_return) {
		printf("adreno8: HWSCHED requires fresh HFI tables, keep=1 and return=0\n");
		adreno8_hwsched_memory_failed();
		return false;
	}
	adreno8_hwsched_attempted = true;
	/* Failed or missing ACKs leave firmware state uncertain until cold reset. */
	adreno8_reset_required = true;
	for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); i++) {
		if (!adreno8_hfi_send(11, features[i], 3, "HWSCHED startup feature")) {
			return false;
		}
	}
	if (!adreno8_hfi_send(14, &handle, 1, "HWSCHED core firmware") ||
	    !adreno8_hfi_send(11, preemption, 3, "disable HWSCHED preemption") ||
	    !adreno8_hfi_send(11, aqe, 3, "enable AQE") ||
	    !adreno8_hfi_send(10, NULL, 0, "HWSCHED START")) {
		return false;
	}
	if (!adreno8_clock_started()) {
		adreno8_hwsched_memory_failed();
		return false;
	}
	adreno8_hwsched_started = true;
	printf("adreno8: HWSCHED START acknowledged; GPU execution initialization is pending\n");
	return true;
}

bool
adreno8_hwsched_raw(const uint32_t *pm4, uint32_t dwords, const char *name)
{
	if (!adreno8_hwsched_ready() || pm4 == NULL || dwords == 0 || dwords >= 255) {
		adreno8_hwsched_memory_failed();
		return false;
	}
	adreno8_reset_required = true;
	if (!adreno8_hfi_send(131, pm4, dwords, name)) {
		adreno8_hwsched_started = false;
		return false;
	}
	return true;
}
