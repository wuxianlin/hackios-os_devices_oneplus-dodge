/* SPDX-License-Identifier: GPL-2.0-only */
/* Standard gen8 HWSCHED context path, following adreno_gen8_hwsched_hfi.c and
 * adreno_hfi.h at OnePlus graphics-kernel revision
 * d50b305f7da9e14715a25120a4ac7b1a4b8b97c3. Preemption, LPAC, hardware fences
 * and external sync objects are disabled. All memory remains until cold reset. */
#include "adreno8_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_gpu_private.h"
#include "adreno8_context_private.h"

#define printf adreno8_say
#define CONTEXT_ID 1u
#define CONTEXT_BYTES 4096u
#define CONTEXT_HEADER_WORDS 12u
#define CONTEXT_QUEUE_WORDS (CONTEXT_BYTES / 4 - CONTEXT_HEADER_WORDS)
#define CONTEXT_SOP 0u
#define CONTEXT_EOP 8u
/* NO_GMEM_ALLOC, PREAMBLE, PER_CONTEXT_TS, NOTIFY, priority 1 (RB0), GL. */
#define CONTEXT_FLAGS 0x00101072u
#define CONTEXT_NOTIFY (1u << 5)
#define CONTEXT_WAIT_NS 200000000ULL
#define CONTEXT_INFLIGHT_MAX 8u
#define CONTEXT_CONSUME_NS 20000000ULL

enum context_state { CONTEXT_COLD, CONTEXT_PREPARED, CONTEXT_REGISTERED, CONTEXT_FAILED };
static enum context_state context_state;
static struct adreno8_memory *context_queue, *context_timestamps, *test_ib, *test_result;
static vm_offset_t context_queue_view, context_timestamp_view;
static uint32_t context_last, context_sequence;
static bool context_tested;

static bool
context_fail(const char *what)
{
	if (context_state == CONTEXT_FAILED) return false;
	context_state = CONTEXT_FAILED;
	adreno8_reset_required = true;
	adreno8_hwsched_memory_failed();
	printf("adreno8: context %s failed; submissions closed and pages retained until cold reset\n", what);
	return false;
}

static bool
context_health(void)
{
	if (context_state == CONTEXT_FAILED) return false;
	if (!adreno8_gpu_commands_ready() || !adreno8_hwsched_ready() ||
	    !adreno8_gpu_memory_ready() || adreno8_gpu_faulted())
		return context_fail("execution prerequisites");
	return true;
}

bool
adreno8_context_prepare(void)
{
	if (!context_health()) return false;
	if (context_state != CONTEXT_COLD) return true;
	context_queue = adreno8_gmu_alloc("context queue", CONTEXT_BYTES, CONTEXT_BYTES,
	    ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV);
	context_timestamps = adreno8_gpu_alloc("context timestamps", CONTEXT_BYTES, CONTEXT_BYTES,
	    ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV);
	if (!context_queue || !context_timestamps || !adreno8_gpu_space.is_on ||
	    adreno8_gpu_space.table_count == 0 || adreno8_gpu_space.context != 0)
		return context_fail("allocation or GPU page table");
	if (!adreno8_memory_publish(context_queue, 0, CONTEXT_BYTES) ||
	    !adreno8_memory_publish(context_timestamps, 0, CONTEXT_BYTES))
		return context_fail("initial publication");
	context_queue_view = ml_io_map_wcomb(kvtophys(context_queue->memory), CONTEXT_BYTES);
	context_timestamp_view = ml_io_map_wcomb(kvtophys(context_timestamps->memory), CONTEXT_BYTES);
	if (!context_queue_view || !context_timestamp_view)
		return context_fail("uncached views");
	/* After publication, only these aliases access the two shared pages. The
	 * queue's host and GMU fields share cache lines; cached cleans can lose updates. */
	adreno8_wr(context_queue_view + 4, context_queue->gmu_address + CONTEXT_HEADER_WORDS * 4);
	adreno8_wr(context_queue_view + 8, CONTEXT_QUEUE_WORDS);
	adreno8_settle();
	context_state = CONTEXT_PREPARED;
	printf("adreno8: context pages ready: queue GMU %#x timestamps GPU %#x\n",
	    context_queue->gmu_address, context_timestamps->gpu_address);
	return true;
}

static bool
context_register(void)
{
	uint32_t registration[6], pointers[9] = {0};
	uint64_t root;

	if (!adreno8_context_prepare()) return false;
	if (context_state == CONTEXT_REGISTERED) return true;
	/* The existing CB0 uses ASID 0. Context IDR identifies this HFI context. */
	root = adreno8_gpu_space.tables[0].phys;
	registration[0] = CONTEXT_ID;
	registration[1] = CONTEXT_FLAGS;
	registration[2] = (uint32_t)root;
	registration[3] = (uint32_t)(root >> 32);
	registration[4] = CONTEXT_ID;
	registration[5] = adreno8_gpu_space.context;
	if (!adreno8_hfi_send(128, registration, 6, "register context"))
		return context_fail("registration ACK");
	pointers[0] = CONTEXT_ID;
	pointers[1] = context_timestamps->gpu_address + CONTEXT_SOP;
	pointers[3] = context_timestamps->gpu_address + CONTEXT_EOP;
	/* RB0 has no user context record; version and optional pointers stay zero. */
	pointers[8] = context_queue->gmu_address;
	if (!adreno8_hfi_send(134, pointers, 9, "context pointers"))
		return context_fail("pointer ACK");
	context_state = CONTEXT_REGISTERED;
	return true;
}

static bool
context_queue_indices(uint32_t *read, uint32_t *write)
{
	*read = adreno8_rd(context_queue_view + 20);
	*write = adreno8_rd(context_queue_view + 24);
	if (adreno8_rd(context_queue_view) != 0 ||
	    adreno8_rd(context_queue_view + 4) != context_queue->gmu_address + CONTEXT_HEADER_WORDS * 4 ||
	    adreno8_rd(context_queue_view + 8) != CONTEXT_QUEUE_WORDS ||
	    *read >= CONTEXT_QUEUE_WORDS || *write >= CONTEXT_QUEUE_WORDS || ((*read | *write) & 3))
		return context_fail("queue indices or header");
	adreno8_settle();
	return true;
}

bool
adreno8_context_done(uint32_t timestamp)
{
	uint32_t read, write;
	if (!timestamp || !context_last || (int32_t)(context_last - timestamp) < 0 ||
	    !context_health()) return false;
	if (!adreno8_hfi_poll()) return context_fail("completion transport");
	if (!context_queue_indices(&read, &write)) return false;
	adreno8_settle();
	uint32_t eop = adreno8_rd(context_timestamp_view + CONTEXT_EOP);
	if ((int32_t)(context_last - eop) < 0) return context_fail("EOP exceeds submitted timestamp");
	return (int32_t)(eop - timestamp) >= 0;
}

static bool
context_wait(uint32_t timestamp)
{
	uint64_t start = mach_absolute_time(), elapsed;
	while (!adreno8_context_done(timestamp)) {
		if (context_state == CONTEXT_FAILED) return false;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= CONTEXT_WAIT_NS) return context_fail("EOP timeout");
		adreno8_sleep_us(100);
	}
	return true;
}

/* The GMU may advance the queue read index after EOP and GPU idle are already
 * visible (seen on device at test pass 2), so the read index gets a short wait. */
static bool
context_consumed(void)
{
	uint64_t start = mach_absolute_time(), elapsed;
	uint32_t read, write;

	for (;;) {
		if (!context_queue_indices(&read, &write)) return false;
		if (read == write) return true;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= CONTEXT_CONSUME_NS) {
			printf("adreno8: context queue read %u, write %u after %llu us\n", read, write, elapsed / 1000);
			return false;
		}
		adreno8_sleep_us(100);
	}
}

/* How many submissions may be outstanding at once: adreno8_inflight, 1 to CONTEXT_INFLIGHT_MAX. */
uint32_t
adreno8_context_depth(void)
{
	return adreno8_inflight >= 1 && adreno8_inflight <= CONTEXT_INFLIGHT_MAX ? adreno8_inflight : 1;
}

/* Waits until the GMU has read enough of the queue for `words` more; returns the write index. */
static bool
context_room(uint32_t words, uint32_t *write)
{
	uint64_t start = mach_absolute_time(), elapsed;
	uint32_t read, empty;

	for (;;) {
		if (!context_queue_indices(&read, write)) return false;
		empty = *write >= read ? CONTEXT_QUEUE_WORDS - (*write - read) : read - *write;
		if (empty > words) return true;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		if (elapsed >= CONTEXT_WAIT_NS) return context_fail("queue full");
		adreno8_sleep_us(100);
	}
}

bool
adreno8_context_wait_idle(const char *what)
{
	if (!context_tested) {
		printf("adreno8: context %s requires the standard context test first\n", what);
		return false;
	}
	if (!context_health()) return false;
	if ((context_last && !context_wait(context_last)) || !adreno8_gpu_wait_idle(what))
		return context_fail("external buffer idle");
	return true;
}

uint32_t
adreno8_context_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t msg[8 + ADRENO8_CONTEXT_MAX_IBS * 3] = {0};
	uint32_t write, length, aligned, timestamp, depth;

	/* Validate before registration or queue publication. IB_PRIV stays clear;
	 * the HFI descriptor encodes each IB length in bytes. */
	if (!addresses || !words || !count || count > ADRENO8_CONTEXT_MAX_IBS) {
		context_fail("IB count");
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!addresses[i] || (addresses[i] & 3) || !words[i] || words[i] > 0xfffff ||
		    addresses[i] >= (1ULL << 32) || addresses[i] + (uint64_t)words[i] * 4 > (1ULL << 32)) {
			context_fail("IB range");
			return 0;
		}
	}
	/* Up to `depth` submissions are outstanding: Mesa records and submits the next batch
	 * while the GPU executes earlier ones. With `depth` outstanding, the oldest retires first. */
	depth = adreno8_context_depth();
	if (!context_register() || (context_last >= depth && !context_wait(context_last - depth + 1))) return 0;
	length = 8 + count * 3;
	aligned = (length + 3) & ~3u;
	if (!context_room(aligned, &write)) return 0;
	timestamp = context_last + 1;
	if (!timestamp) timestamp = 1;
	context_sequence = (context_sequence + 1) & 0xfff;
	msg[0] = 130u | length << 8 | context_sequence << 20;
	msg[1] = CONTEXT_ID;
	msg[2] = CONTEXT_NOTIFY;
	msg[3] = timestamp;
	msg[6] = count;
	for (uint32_t i = 0; i < count; i++) {
		msg[8 + i * 3] = (uint32_t)addresses[i];
		msg[9 + i * 3] = (uint32_t)(addresses[i] >> 32);
		msg[10 + i * 3] = words[i] * 4;
	}
	adreno8_reset_required = true;
	for (uint32_t i = 0; i < aligned; i++) {
		adreno8_wr(context_queue_view + CONTEXT_HEADER_WORDS * 4 + write * 4,
		    i < length ? msg[i] : 0xfafafafa);
		write = (write + 1) % CONTEXT_QUEUE_WORDS;
	}
	adreno8_settle();
	adreno8_wr(context_queue_view + 24, write);
	adreno8_settle();
	/* No sync objects or preemption: normal RB0 dispatch uses interrupt bit 2. */
	context_last = timestamp;
	adreno8_wr(adreno8_gmu.base + GMU_HOST2GMU_SET, 1u << 2);
	return timestamp;
}

uint32_t
adreno8_context_last(void)
{
	return context_last;
}

bool
adreno8_context_close(void)
{
	uint32_t unregister[2] = {CONTEXT_ID, context_last};
	if (!context_health()) return false;
	if (context_state != CONTEXT_REGISTERED) return context_state == CONTEXT_PREPARED;
	if ((context_last && !context_wait(context_last)) ||
	    !adreno8_gpu_wait_idle("context unregister idle") ||
	    !adreno8_hfi_send(129, unregister, 2, "unregister context") ||
	    !adreno8_gpu_wait_idle("context unregister completion"))
		return context_fail("unregister");
	/* Firmware no longer owns the queue. Reuse pages and monotonic timestamps
	 * after a later registration; retain all physical memory and aliases. */
	uint32_t write = adreno8_rd(context_queue_view + 24);
	adreno8_wr(context_queue_view + 20, write);
	adreno8_settle();
	context_state = CONTEXT_PREPARED;
	return true;
}

bool
adreno8_context_test(void)
{
	if (test_ib || test_result || !adreno8_context_prepare())
		return context_fail("test prerequisites");
	test_ib = adreno8_gpu_alloc("context test IB", CONTEXT_BYTES, CONTEXT_BYTES,
	    ADRENO8_MEM_READ | ADRENO8_MEM_EXEC);
	test_result = adreno8_gpu_alloc("context test result", CONTEXT_BYTES, CONTEXT_BYTES,
	    ADRENO8_MEM_READ | ADRENO8_MEM_WRITE);
	if (!test_ib || !test_result) return context_fail("test allocation");
	for (uint32_t pass = 1; pass <= 2; pass++) {
		uint32_t expected = 0x22c00000u | pass;
		uint32_t *ib = (uint32_t *)test_ib->memory;
		uint32_t count = adreno8_pm4_scratch(ib, CONTEXT_BYTES / 4, test_result->gpu_address, expected);
		uint64_t address = test_ib->gpu_address;
		if (!count || !adreno8_memory_publish(test_ib, 0, count * 4) ||
		    !adreno8_memory_publish(test_result, 0, CONTEXT_BYTES))
			return context_fail("test publication");
		uint32_t timestamp = adreno8_context_submit(&address, &count, 1);
		if (!timestamp || !context_wait(timestamp) ||
		    !adreno8_gpu_wait_idle("context test idle") ||
		    !adreno8_memory_acquire(test_result, 0, CONTEXT_BYTES))
			return context_fail("test completion");
		volatile uint32_t *result = (volatile uint32_t *)test_result->memory;
		if (result[0] != expected || adreno8_rd(context_timestamp_view + CONTEXT_SOP) != timestamp ||
		    adreno8_rd(context_timestamp_view + CONTEXT_EOP) != timestamp)
			return context_fail("test result or timestamps");
		for (unsigned i = 1; i < CONTEXT_BYTES / 4; i++)
			if (result[i] != 0) return context_fail("test result bounds");
		if (!context_health() || !adreno8_hfi_poll()) return context_fail("test fault check");
		if (!context_consumed())
			return context_fail("test queue consumption");
		printf("adreno8: context %u pass %u: SOP/EOP %u result %#x; idle and fault checks passed\n",
		    CONTEXT_ID, pass, timestamp, result[0]);
	}
	if (!adreno8_context_close()) return false;
	context_tested = true;
	printf("adreno8: standard context reused for two non-privileged IBs and unregistered; resources retained until cold reset\n");
	return true;
}
