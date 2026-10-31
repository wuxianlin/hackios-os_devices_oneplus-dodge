/* SM8750 GMU HFI queues, resource tables and GX diagnostics. */
#include "adreno8_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_memory_private.h"
#include "adreno8_clock_private.h"
#include <sys/hakios.input.h>

#define printf adreno8_say

static unsigned adreno8_hfi_seq;
static bool adreno8_ordinary_attempted, adreno8_ordinary_started;
bool adreno8_hfi_tables_sent;

static vm_offset_t
adreno8_queue_header(unsigned queue)
{
	return adreno8_hfi_view_table + ADRENO8_HFI_HEADER(queue) * 4;
}

static bool
adreno8_queue_indices(vm_offset_t header, uint32_t *read, uint32_t *write)
{
	*read = adreno8_rd(header + 40);
	*write = adreno8_rd(header + 44);
	return adreno8_rd(header) == 1 &&
	       adreno8_rd(header + 12) == ADRENO8_HFI_QUEUE_DWORDS &&
	       *read < ADRENO8_HFI_QUEUE_DWORDS && *write < ADRENO8_HFI_QUEUE_DWORDS &&
	       ((*read | *write) & 3) == 0;
}

static bool
adreno8_queue_write(const uint32_t *msg, uint32_t dwords)
{
	vm_offset_t header = adreno8_queue_header(0);
	uint32_t read, write, space, aligned = (dwords + 3) & ~3u;

	if (dwords == 0 || dwords > ADRENO8_HFI_MAX_DWORDS ||
	    !adreno8_queue_indices(header, &read, &write)) {
		return false;
	}
	space = (read + ADRENO8_HFI_QUEUE_DWORDS - write - 1) % ADRENO8_HFI_QUEUE_DWORDS;
	if (space < aligned) {
		printf("adreno8: %u: command queue has %u free words; needs %u\n", adreno8_step, space, aligned);
		return false;
	}
	/* The unused slot distinguishes full from empty; padding is part of the allocation. */
	for (uint32_t i = 0; i < aligned; i++) {
		adreno8_wr(adreno8_hfi_view_cmd + 4 * write, i < dwords ? msg[i] : 0xfafafafa);
		write = (write + 1) % ADRENO8_HFI_QUEUE_DWORDS;
	}
	adreno8_settle();
	bool raw = (msg[0] & 0x000f00ffu) == 131;
	if (raw) adreno8_announce("RAW131 payload ready: %u dwords; producer publication follows", dwords);
	/* Firmware can observe the published producer before notification. Keep
	 * diagnostics outside the vendor's producer/barrier/doorbell sequence. */
	adreno8_wr(header + 44, write);
	adreno8_settle();
	adreno8_wr(adreno8_gmu.base + GMU_HOST2GMU_SET, 1);
	if (raw) printf("adreno8: RAW131 producer %u and doorbell writes returned\n", write);
	return true;
}

/* Return the actual message length, zero for an empty queue, or -1 for a corrupt queue. */
static int
adreno8_queue_read(unsigned queue, uint32_t *msg)
{
	vm_offset_t header = adreno8_queue_header(queue);
	vm_offset_t data = queue == 1 ? adreno8_hfi_view_resp : adreno8_hfi_view_debug;
	uint32_t read, write, dwords, aligned, used;

	if (!adreno8_queue_indices(header, &read, &write)) {
		return -1;
	}
	if (read == write) {
		return 0;
	}
	/* Read the producer index before reading its message data through the uncached alias. */
	adreno8_settle();
	msg[0] = adreno8_rd(data + 4 * read);
	dwords = (msg[0] >> 8) & 0xff;
	aligned = (dwords + 3) & ~3u;
	used = (write + ADRENO8_HFI_QUEUE_DWORDS - read) % ADRENO8_HFI_QUEUE_DWORDS;
	if (dwords == 0 || aligned > used) {
		return -1;
	}
	for (uint32_t i = 1; i < dwords; i++) {
		msg[i] = adreno8_rd(data + 4 * ((read + i) % ADRENO8_HFI_QUEUE_DWORDS));
	}
	adreno8_settle();
	adreno8_wr(header + 40, (read + aligned) % ADRENO8_HFI_QUEUE_DWORDS);
	adreno8_settle();
	return (int)dwords;
}

static bool
adreno8_hfi_reply(uint32_t id, const uint32_t *payload, uint32_t count)
{
	uint32_t msg[11];

	if (count == 0 || count > 10) {
		return false;
	}
	adreno8_hfi_seq = (adreno8_hfi_seq + 1) & 0xfff;
	msg[0] = (adreno8_hfi_seq << 20) | (1u << 16) | ((count + 1) << 8) | id;
	memcpy(msg + 1, payload, count * sizeof(*payload));
	return adreno8_queue_write(msg, count + 1);
}

static bool
adreno8_hfi_failed(void)
{
	if (adreno8_direct_ring) adreno8_ordinary_attempted = true;
	adreno8_ordinary_started = false;
	adreno8_hwsched_memory_failed();
	return false;
}

static bool
adreno8_hfi_health(void)
{
	uint32_t irq = adreno8_rd(adreno8_gmu.base + GMU_GMU2HOST_INFO);
	uint32_t init = adreno8_rd(adreno8_gmu.base + GMU_FW_INIT_RESULT);

	adreno8_wr(adreno8_gmu.base + GMU_GMU2HOST_CLR, irq & 5u);
	if ((irq & (1u << 15)) || (init & GMU_FW_FAILED)) {
		printf("adreno8: %u: GMU fault during HFI: interrupt %#x init %#x\n", adreno8_step, irq, init);
		return false;
	}
	if ((adreno8_direct_ring || adreno8_gpu8_secure_mode == SECURE_MODE_TRANSLATION_B) && adreno8_gpu_faulted()) {
		printf("adreno8: HFI stopped after an address-space fault\n");
		return false;
	}
	return true;
}

/*
 * How long the GPU worked, from the GMU's TS_RETIRE notifications (kgsl's hfi_ts_retire_cmd:
 * header, context, timestamp, type, then the 64-bit always-on counts at 19.2 MHz submitted_to_rb,
 * sop, eop, retired_on_gmu and active, then a version). The time is eop - sop, which kgsl reports
 * as the work's active time for GMU firmware older than HFI minor 4.
 */
static uint64_t retired_ticks;
uint64_t adreno8_retired_total;                 /* never reset: adreno8_speed_tick()'s measure of load */
static uint32_t retired_count, retired_by[7];

static void
adreno8_retire_note(const uint32_t *msg)
{
	static const uint64_t limits_us[6] = {100, 250, 500, 1000, 2000, 4000};
	uint64_t sop = msg[6] | (uint64_t)msg[7] << 32, eop = msg[8] | (uint64_t)msg[9] << 32;
	uint64_t ticks = eop > sop ? eop - sop : 0;
	unsigned bucket = 0;

	while (bucket < 6 && ticks * 10 / 192 >= limits_us[bucket]) bucket++;
	retired_by[bucket]++;
	retired_count++;
	retired_ticks += ticks;
	adreno8_retired_total += ticks;
}

/* What the notifications said since this was last asked (redwood_gpu_worked's contract). */
void
adreno8_hfi_worked(uint64_t *microseconds, uint32_t *count, uint32_t by[7])
{
	*microseconds = retired_ticks * 10 / 192;
	*count = retired_count;
	memcpy(by, retired_by, sizeof(retired_by));
	memset(retired_by, 0, sizeof(retired_by));
	retired_ticks = 0;
	retired_count = 0;
}

/* The ACK waiter and EOP polling use the same message handling. A bounded drain
 * keeps a producer that fills a queue continuously from extending a timeout. */
static bool
adreno8_hfi_pump(uint32_t request, bool *ack, unsigned *notices)
{
	uint32_t resp[ADRENO8_HFI_MAX_DWORDS], id = request & 0xff;

	if (!adreno8_hfi_health()) return false;
	for (unsigned queue = 2; queue != 0; queue--) {
		for (unsigned seen = 0; seen < ADRENO8_HFI_QUEUE_DWORDS / 4; seen++) {
			int words = adreno8_queue_read(queue, resp);
			uint32_t type, received_id;

			if (words == 0) break;
			if (words < 0) {
				printf("adreno8: %u: HFI queue %u has invalid indices or message length\n", adreno8_step, queue);
				return false;
			}
			type = (resp[0] >> 16) & 0xf;
			received_id = resp[0] & 0xff;
			if (type == 1) {
				if (words < 3) {
					printf("adreno8: %u: short HFI ACK (%d words)\n", adreno8_step, words);
					return false;
				}
				/* Gen8 echoes the full header; warmboot recording may change bit 19. */
				if (!request || queue != 1 || (resp[1] & ~(1u << 19)) != request) {
					if ((*notices)++ < 8)
						printf("adreno8: %u: unrelated HFI ACK %#x; waiting for %#x\n", adreno8_step, resp[1], request);
					continue;
				}
				/* FEATURE_CTRL uses the v5 data/error ACK; startup requires both to be zero. */
				if (id == 11) {
					if (words < 4) {
						printf("adreno8: %u: short FEATURE_CTRL ACK (%d words)\n", adreno8_step, words);
						return false;
					}
					printf("adreno8: %u: HFI 11 ACK: data %#x error %#x\n", adreno8_step, resp[2], resp[3]);
					if (resp[3] != 0) return false;
				}
				/* These HWSCHED messages share gen8_hfi_send_cmd_async's status contract. */
				bool hwsched = !adreno8_direct_ring &&
				    (id == 10 || id == 128 || id == 129 || id == 131 || id == 134);
				if (hwsched ? resp[2] == 0xffffffffu : resp[2] != 0) {
					printf("adreno8: %u: HFI %u failed: %#x\n", adreno8_step, id, resp[2]);
					return false;
				}
				*ack = true;
			} else if (type == 0 && (received_id == 100 || received_id == 150)) {
				printf("adreno8: %u: HFI fatal message %u data %#x (%d words)\n",
				    adreno8_step, received_id, words >= 2 ? resp[1] : 0, words);
				return false;
			} else if (type == 0 && received_id == 133) {
				/* EOP memory remains the completion check; this packet is a notification. */
				if (queue != 1 || words != 15) return false;
				adreno8_retire_note(resp);
			} else if (type == 0 && received_id == 101 && words >= 4) {
				if ((*notices)++ < 8)
					printf("adreno8: %u: GMU debug type %u timestamp %u data %#x\n", adreno8_step, resp[1], resp[2], resp[3]);
			} else if (type == 0 && (received_id == 20 || received_id == 110)) {
				uint32_t reply[10], reply_words = 0;
				if (adreno8_direct_ring || id != 10 || queue != 1 ||
				    !adreno8_hwsched_request(resp, (uint32_t)words, reply, &reply_words) ||
				    !adreno8_hfi_reply(received_id, reply, reply_words)) {
					printf("adreno8: START resource request %u failed (%d words)\n", received_id, words);
					return false;
				}
			} else {
				if (id == 10 && queue == 1) {
					printf("adreno8: unexpected START response %#x (%d words)\n", resp[0], words);
					return false;
				}
				if ((*notices)++ < 8)
					printf("adreno8: %u: unhandled HFI header %#x (%d words)\n", adreno8_step, resp[0], words);
			}
		}
	}
	return adreno8_hfi_health();
}

bool
adreno8_hfi_poll(void)
{
	unsigned notices = 0;
	bool ack = false;
	if (adreno8_direct_ring && adreno8_ordinary_attempted && !adreno8_ordinary_started)
		return adreno8_hfi_failed();
	if (adreno8_gmu.base == 0 || adreno8_hfi_view_table == 0 ||
	    adreno8_hfi_view_resp == 0 || adreno8_hfi_view_debug == 0 ||
	    !adreno8_hfi_pump(0, &ack, &notices)) {
		adreno8_hfi_failed();
		adreno8_gmu_post_mortem();
		return false;
	}
	return true;
}

bool
adreno8_hfi_send(uint32_t id, const uint32_t *payload, uint32_t count, const char *what)
{
	uint32_t msg[ADRENO8_HFI_MAX_DWORDS];
	uint64_t start, elapsed = 0;
	unsigned notices = 0;

	if (adreno8_direct_ring && adreno8_ordinary_attempted && !adreno8_ordinary_started)
		return adreno8_hfi_failed();
	if (id > 0xff || count >= ADRENO8_HFI_MAX_DWORDS || (count != 0 && payload == NULL) ||
	    ((adreno8_direct_ring || adreno8_gpu8_secure_mode == SECURE_MODE_TRANSLATION_B) && adreno8_gpu_faulted())) {
		return adreno8_hfi_failed();
	}
	adreno8_hfi_seq = (adreno8_hfi_seq + 1) & 0xfff;
	msg[0] = (adreno8_hfi_seq << 20) | ((count + 1) << 8) | id;
	for (uint32_t i = 0; i < count; i++) msg[i + 1] = payload[i];
	if (!adreno8_queue_write(msg, count + 1)) {
		printf("adreno8: %u: could not queue HFI %u (%s)\n", adreno8_step, id, what);
		return adreno8_hfi_failed();
	}
	/* Start the ACK budget after queue publication and its optional diagnostic pauses. */
	start = mach_absolute_time();
	for (;;) {
		bool ack = false;
		absolutetime_to_nanoseconds(mach_absolute_time() - start, &elapsed);
		/* START includes synchronous memory mapping work; its total budget is five seconds. */
		if (elapsed >= (id == 10 ? 5000000000ULL : 1000000000ULL)) {
			printf("adreno8: %u: HFI %u (%s) sequence %u timed out\n", adreno8_step, id, what, msg[0] >> 20);
			break;
		}
		if (!adreno8_hfi_pump(msg[0], &ack, &notices)) break;
		if (ack) return true;
		adreno8_sleep_us(100);
	}
	adreno8_hfi_failed();
	adreno8_gmu_post_mortem();
	return false;
}

/* ---- step 12: the queues ----------------------------------------------------------------------------------- */

bool
adreno8_step_queues(void)
{
	adreno8_hfi_tables_sent = false;
	if (!adreno8_cx_ready()) {
		return false;
	}
	if (!adreno8_gmu_runs) {
		printf("adreno8: %u: no firmware running to talk to: step 11\n", adreno8_step);
		return false;
	}
	adreno8_announce("initialize the gen8 HFI queues and poll status for 500 ms");
	if (adreno8_hfi_view_table == 0 || adreno8_hfi_view_cmd == 0 ||
	    adreno8_hfi_view_resp == 0 || adreno8_hfi_view_debug == 0) {
		printf("adreno8: %u: HFI queue mappings are incomplete\n", adreno8_step);
		return false;
	}
	printf("adreno8: %u: what the queues' table says now, read without cache: version %#x; command queue status %u index %u/%u, response queue status %u index %u/%u\n",
	    adreno8_step, adreno8_rd(adreno8_hfi_view_table), adreno8_rd(adreno8_hfi_view_table + ADRENO8_HFI_HEADER(0) * 4), adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(0) + 10) * 4),
	    adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(0) + 11) * 4), adreno8_rd(adreno8_hfi_view_table + ADRENO8_HFI_HEADER(1) * 4), adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(1) + 10) * 4),
	    adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(1) + 11) * 4));
	adreno8_put(&adreno8_gmu, "gmu2host clr", GMU_GMU2HOST_CLR, ~0u);
	adreno8_put(&adreno8_gmu, "gmu2host mask: poll messages and faults", GMU_GMU2HOST_MASK, ~0u);
	adreno8_put(&adreno8_gmu, "hfi ctrl init", GMU_HFI_CTRL_INIT, 1);
	if (!adreno8_wait_for(&adreno8_gmu, GMU_HFI_CTRL_STATUS, 1, 1, 500)) {
		printf("adreno8: %u: half a second and the queues' status word is still %#x\n", adreno8_step,
		    adreno8_rd(adreno8_gmu.base + GMU_HFI_CTRL_STATUS));
		adreno8_gmu_post_mortem();
		return false;
	}
	{
		uint32_t version = adreno8_get(&adreno8_gmu, "DTCM HFI version", GMU_DTCM_VERSIONS + 16);

		/* Gen8 switched the performance table to message 15 in HFI 2.6.1. */
		if ((version >> 28) != 2 || version < 0x20060001) {
			printf("adreno8: %u: unsupported HFI version %#x; requires v2.6.1 or later v2\n", adreno8_step, version);
			return false;
		}
	}
	adreno8_gmu_talks = true;
	printf("adreno8: %u: THE GMU'S MESSAGE QUEUES ARE UP\n", adreno8_step);
	return true;
}

/* ---- step 13: the speed and bandwidth tables ---------------------------------------------------------------- */

/*
 * What the messages carry (Linux: a6xx_gmu_rpmh_votes_init and a6xx_hfi_start's first two
 * messages). Every supply vote comes out of the RPMh static layout read in step 1; the bandwidth
 * rows are made the way a6xx_gmu_rpmh_bw_votes_init makes them, from the same layout, for the
 * static list of bandwidths. The table message is the a8xx one: the flat v1 message is what the
 * older chips get.
 */

static uint32_t adreno8_bw_votes[BW_COUNT][BCM_COUNT];

static bool
adreno8_fill_votes(void)
{
	for (unsigned i = 0; i < adreno8_gx_count; i++) {
		if (!adreno8_arc_vote("gfx.lvl", "gmxc.lvl", adreno8_gx_level[i], &adreno8_gx_votes[i]) ||
		    !adreno8_dep_vote(adreno8_gx_level[i], &adreno8_gx_dep_votes[i])) {
			return false;
		}
	}
	for (unsigned i = 0; i < 3; i++) {
		if (!adreno8_arc_vote("cx.lvl", "mx.lvl", adreno8_gmu_level[i], &adreno8_cx_votes[i])) {
			return false;
		}
	}
	printf("adreno8: %u: %u 3D-core speeds: %llu Hz votes %#x (depends %#x) ... %llu Hz votes %#x (depends %#x)\n", adreno8_step,
	    adreno8_gx_count, (uint64_t)adreno8_gx_hz[1], adreno8_gx_votes[1], adreno8_gx_dep_votes[1], (uint64_t)adreno8_gx_hz[adreno8_gx_count - 1], adreno8_gx_votes[adreno8_gx_count - 1],
	    adreno8_gx_dep_votes[adreno8_gx_count - 1]);
	printf("adreno8: %u: the GMU's own three speeds vote %#x %#x %#x\n", adreno8_step, adreno8_cx_votes[0], adreno8_cx_votes[1], adreno8_cx_votes[2]);
	return true;
}

/* Bandwidth votes follow gen8 build_bw_table and adreno_rpmh.c tcs_cmd_data. */

static bool
adreno8_fill_bw_votes(void)
{
	const struct adreno8_bcm_db *aux[BCM_COUNT];
	unsigned length[BCM_COUNT], bcm_count = 0;

	for (unsigned i = 0; i < BCM_COUNT; i++) {
		const uint8_t *data = NULL;

		if (adreno8_rpmh_lookup(adreno8_bcms[i].name, NULL, NULL, &data, &length[i]) == 0 || length[i] < sizeof(struct adreno8_bcm_db)) {
			printf("adreno8: %u: no RPMh description for \"%s\": no bandwidth table\n", adreno8_step, adreno8_bcms[i].name);
			return false;
		}
		aux[i] = (const struct adreno8_bcm_db *)data;   /* little endian both sides */
		if (!adreno8_bcms[i].fixed && (aux[i]->unit == 0 || aux[i]->width == 0 || adreno8_bcms[i].buswidth == 0)) {
			printf("adreno8: %u: invalid BCM unit or bus width for %s\n", adreno8_step, adreno8_bcms[i].name);
			return false;
		}
		bcm_count++;
	}
	for (unsigned bw_index = 0; bw_index < BW_COUNT; bw_index++) {
		uint32_t bw = adreno8_bw_kb[bw_index];

		for (unsigned i = 0; i < bcm_count; i++) {
			bool commit = i == bcm_count - 1 ||
			    (aux[i + 1] != NULL && aux[i]->vcd != aux[i + 1]->vcd);
			uint64_t peak;
			uint32_t vote;

			if (bw == 0) {
				adreno8_bw_votes[bw_index][i] = BCM_TCS_CMD(commit, false, 0, 0);
				continue;
			}
			if (adreno8_bcms[i].fixed) {
				vote = bw >= adreno8_bcms[i].perfmode_bw ? adreno8_bcms[i].perfmode : 0;
				adreno8_bw_votes[bw_index][i] = BCM_TCS_CMD(commit, true, 0, vote);
				continue;
			}
			peak = (uint64_t)bw * aux[i]->width / adreno8_bcms[i].buswidth;
			peak *= 1000;
			peak /= aux[i]->unit;
			vote = peak < 1 ? 1 : peak > 0x3fff ? 0x3fff : (uint32_t)peak;
			/* AB stays zero until GMU_AB_VOTE support is queried and the table is rebuilt. */
			adreno8_bw_votes[bw_index][i] = BCM_TCS_CMD(commit, true, 0, vote);
		}
	}
	printf("adreno8: %u: %u bandwidth rows for %u monitors: %u KB/s votes %#x %#x %#x ... %u KB/s votes %#x %#x %#x\n",
	    adreno8_step, BW_COUNT, bcm_count, adreno8_bw_kb[0], adreno8_bw_votes[0][0], adreno8_bw_votes[0][1], adreno8_bw_votes[0][2], adreno8_bw_kb[BW_COUNT - 1],
	    adreno8_bw_votes[BW_COUNT - 1][0], adreno8_bw_votes[BW_COUNT - 1][1], adreno8_bw_votes[BW_COUNT - 1][2]);
	return true;
}

bool
adreno8_step_tables(void)
{
	uint32_t tbl[128], bw[159], at, n;
	uint32_t ddr_addr[BCM_COUNT];

	adreno8_hfi_tables_sent = false;
	if (!adreno8_cx_ready()) {
		return false;
	}
	if (!adreno8_gmu_talks) {
		printf("adreno8: %u: nobody answers on the queues yet: step 12\n", adreno8_step);
		return false;
	}
	adreno8_announce("work out every supply vote from the RPMh layout of step 1, and hand the firmware the table of them (Linux: a8xx_hfi_send_perf_table: three words per 3D-core speed, two per GMU speed)");
	if (adreno8_gx_count < 2 || adreno8_gx_count > 17 || BW_COUNT > 16 || BCM_COUNT > 8) {
		printf("adreno8: %u: invalid HFI power table dimensions\n", adreno8_step);
		return false;
	}
	if (!adreno8_fill_votes()) {
		return false;
	}
	at = 0;
	tbl[at++] = 0;                        /* the table's version: v0, the only one */
	tbl[at++] = 1;                        /* HFI_TABLE_GPU_PERF */
	tbl[at++] = adreno8_gx_count;                 /* the 3D core's speeds */
	tbl[at++] = 3;                        /* words per one: the vote, the dependency, the kHz */
	for (unsigned i = 0; i < adreno8_gx_count; i++) {
		tbl[at++] = adreno8_gx_votes[i];
		tbl[at++] = adreno8_gx_dep_votes[i];
		tbl[at++] = adreno8_gx_hz[i] / 1000;
	}
	tbl[at++] = 3;                        /* the GMU's own speeds */
	tbl[at++] = 2;
	for (unsigned i = 0; i < 3; i++) {
		tbl[at++] = adreno8_cx_votes[i];
		tbl[at++] = adreno8_gmu_hz[i] / 1000;
	}
	n = at;
	if (!adreno8_hfi_send(15, tbl, n, "here is how to vote for power at every speed")) {
		return false;
	}
	printf("adreno8: %u: the speed table is in (%u words)\n", adreno8_step, n);

	adreno8_announce("and the bandwidth table: %u rows of the three memory monitors' RPMh words, the CN0 gate on and off, and the wait bits (Linux: a6xx_generate_bw_table, verbatim in shape)", BW_COUNT);
	if (!adreno8_fill_bw_votes()) {
		return false;
	}
	for (unsigned i = 0; i < BCM_COUNT; i++) {
		ddr_addr[i] = adreno8_rpmh_lookup(adreno8_bcms[i].name, NULL, NULL, NULL, NULL);
		if (ddr_addr[i] == 0) {
			printf("adreno8: %u: RPMh has no address for \"%s\"\n", adreno8_step, adreno8_bcms[i].name);
			return false;
		}
	}
	{
		uint32_t cn0 = adreno8_rpmh_lookup("CN0", NULL, NULL, NULL, NULL);

		if (cn0 == 0) {
			printf("adreno8: %u: RPMh has no \"CN0\": the firmware's memory gate is unknown\n", adreno8_step);
			return false;
		}
		memset(bw, 0, sizeof(bw));
		bw[0] = BW_COUNT;               /* bandwidth levels */
		bw[1] = 1;                      /* one CX monitor: CN0 */
		bw[2] = BCM_COUNT;              /* the three DDR-side ones */
		bw[3] = 1;                      /* wait for CN0's commit: row 0 has it */
		bw[4] = 0;
		for (unsigned j = 0; j < BCM_COUNT; j++) {
			if (adreno8_bw_votes[0][j] & (1u << 30)) {
				bw[4] |= 1u << j;   /* the commit bits of the first row, per Linux */
			}
		}
		bw[5] = cn0;                    /* CN0's address */
		bw[11] = BCM_TCS_CMD(true, false, 0, 0);      /* its off word */
		bw[17] = BCM_TCS_CMD(true, true, 0, 1u << 0); /* and its on word */
		for (unsigned j = 0; j < BCM_COUNT; j++) {
			bw[23 + j] = ddr_addr[j];
		}
		for (unsigned b = 0; b < BW_COUNT; b++) {
			for (unsigned j = 0; j < BCM_COUNT; j++) {
				bw[31 + b * 8 + j] = adreno8_bw_votes[b][j];
			}
		}
		if (!adreno8_hfi_send(3, bw, 159, "here is how to vote for bandwidth")) {
			return false;
		}
		printf("adreno8: %u: the bandwidth table is in\n", adreno8_step);
	}
	printf("adreno8: %u: THE FIRMWARE HAS ITS ORDERS: %u speeds, %u bandwidth levels\n", adreno8_step, adreno8_gx_count, BW_COUNT);
	adreno8_hfi_tables_sent = true;
	return true;
}

/* ---- step 15: minimum DCVS request --------------------------------------------------------------- */

/* Minimum votes follow START and precede OOB_GPU.
 * All six live K90 DT bins map 160 MHz to DDR index 2. This probe does not access GX registers. */
bool
adreno8_step_gx_power(void)
{
	uint32_t msg[3] = {1, 1, 2}; /* Blocking ACK, 160 MHz, DDR index 2; AB=0. adreno8_speed_tick() raises it later. */
	uint32_t power;

	if (!adreno8_cx_ready() || !adreno8_gmu_talks ||
	    !(adreno8_direct_ring ? adreno8_ordinary_ready() : adreno8_hwsched_ready())) {
		return false;
	}
	if (!adreno8_gpu8_keep || adreno8_gpu8_return || adreno8_gx_count < 2 ||
	    adreno8_gx_hz[1] != 160000000 || adreno8_speed_asked >= adreno8_gx_count) {
		printf("adreno8: %u: DCVS probe requires keep=1, return=0 and the 160 MHz table; finish with host cold reset\n", adreno8_step);
		return false;
	}
	adreno8_announce("request 160 MHz and bandwidth index 2; retain resources until host cold reset");
	/* A missing ACK cannot prove that the firmware left its power votes unchanged. */
	adreno8_reset_required = true;
	if (!adreno8_hfi_send(30, msg, 3, "minimum DCVS vote")) {
		return false;
	}
	power = adreno8_get(&adreno8_gmu, "power clock status after DCVS", GMU_PWR_CLK_STATUS);
	adreno8_get(&adreno8_gmu, "RPMh power state after DCVS", GMU_RPMH_POWER_STATE);
	printf("adreno8: %u: DCVS acknowledged; GX status %#x (%s); GPU execution remains pending\n",
	    adreno8_step, power, GMU_PWR_GX_UP(power) ? "power and clock on" : "power or clock off");
	return true;
}

/*
 * adreno8_speed=N above 1: the GPU's speed follows its load between speed 1 and speed N of
 * adreno8_boot_speeds (1 the slowest). Called with each submission under RedwoodGPU's lock and
 * acting at most every 100 ms, on the GPU time of the submissions retired since (TS_RETIRE):
 *  - up to N at once when that time, scaled to speed 1, is 20% of the interval or more, or within
 *    300 ms of a touch (annibale_cpufreq.c's boost);
 *  - back to speed 1 once it would have stayed below 10% at speed 1 for 500 ms;
 *  - speed 1 while any GPU sensor reads 70 C or more, or none reads, until all are below 60 C
 *    (the rule of annibale_cpufreq.c for the processors).
 * A vote the GMU does not acknowledge ends the changes for the run.
 */
#define SPEED_TICK_NS           100000000ull
#define SPEED_UP_PERMILLE       200
#define SPEED_DOWN_PERMILLE     100
#define SPEED_DOWN_NS           500000000ull
#define SPEED_TOUCH_NS          300000000ull
#define SPEED_HOT_TENTHS        700
#define SPEED_COOL_TENTHS       600

static uint32_t adreno8_speed_now = 1, adreno8_speed_changes;
static uint64_t adreno8_speed_ticked, adreno8_speed_busy_seen, adreno8_speed_light_since;
static bool adreno8_speed_hot;

/* For RedwoodGPU's statistics: the speed voted last (the step 15 vote until a change) and the changes since. */
void
adreno8_speed_state(uint32_t *mhz, uint32_t *changes)
{
	*mhz = adreno8_gx_count > 1 ? adreno8_gx_hz[adreno8_speed_now] / 1000000 : 0;
	*changes = adreno8_speed_changes;
}

void
adreno8_speed_tick(void)
{
	uint64_t now = mach_absolute_time(), since, light, touched, touch_age, permille, busy_ns;
	uint32_t want = adreno8_speed_now, msg[3];
	int32_t tenths = 0;
	bool reading;

	if (adreno8_speed_asked <= 1 || adreno8_speed_asked >= adreno8_gx_count || !adreno8_hfi_tables_sent) {
		return;
	}
	absolutetime_to_nanoseconds(now - adreno8_speed_ticked, &since);
	if (adreno8_speed_ticked != 0 && since < SPEED_TICK_NS) {
		return;
	}
	busy_ns = (adreno8_retired_total - adreno8_speed_busy_seen) * 10000 / 192;
	permille = adreno8_speed_ticked != 0 && since != 0 ?
	    busy_ns * 1000 / since * adreno8_gx_hz[adreno8_speed_now] / adreno8_gx_hz[1] : 0;
	adreno8_speed_ticked = now;
	adreno8_speed_busy_seen = adreno8_retired_total;
	/* the K90's cpufreq driver (touch-boost + thermal) is not ported to
	 * this phone yet: no boost, and no temperature reading -- with no
	 * reading the governor takes the conservative hot state (the K90's own
	 * behaviour); port annibale_cpufreq.c and delete this block */
	touched = 0;
	reading = false;
	tenths = SPEED_HOT_TENTHS;
	if (!reading || tenths >= SPEED_HOT_TENTHS) {
		adreno8_speed_hot = true;
	} else if (tenths < SPEED_COOL_TENTHS) {
		adreno8_speed_hot = false;
	}
	if (adreno8_speed_hot) {
		want = 1;
	} else if (permille >= SPEED_UP_PERMILLE || (touched != 0 && touched <= now && touch_age < SPEED_TOUCH_NS)) {
		want = adreno8_speed_asked;
		adreno8_speed_light_since = 0;
	} else if (permille < SPEED_DOWN_PERMILLE) {
		if (adreno8_speed_light_since == 0) {
			adreno8_speed_light_since = now;
		}
		absolutetime_to_nanoseconds(now - adreno8_speed_light_since, &light);
		if (light >= SPEED_DOWN_NS) {
			want = 1;
		}
	} else {
		adreno8_speed_light_since = 0;
	}
	if (want == adreno8_speed_now) {
		return;
	}
	msg[0] = 1;
	msg[1] = want;
	msg[2] = adreno8_speeds[want - 1].bus;
	if (!adreno8_hfi_send(30, msg, 3, "GPU speed")) {
		printf("adreno8: the GPU speed vote for %u MHz was not acknowledged; staying at %u MHz\n",
		    adreno8_gx_hz[want] / 1000000, adreno8_gx_hz[adreno8_speed_now] / 1000000);
		adreno8_speed_asked = 1;
		return;
	}
	adreno8_speed_now = want;
	adreno8_speed_changes++;
}

/* ---- step 16: the last look ------------------------------------------------------------------------------------ */

bool
adreno8_step_last_look(void)
{
	uint32_t ao, fence, power;

	adreno8_announce("read GMU fault, interrupt, power, counter and queue status");
	if (adreno8_space_faulted(&adreno8_gmu_space)) {
		printf("adreno8: %u: NOTE: something was refused translation since we turned it on; the steps above had their answers anyway\n", adreno8_step);
	}
	ao = adreno8_get(&adreno8_gmu, "ao interrupt status", GMU_AO_INTERRUPT_STATUS);
	if (ao != 0) {
		printf("adreno8: %u: the GMU's urgent conditions: %#x%s%s%s\n", adreno8_step, ao, ao & 1 ? " [watchdog bit]" : "",
		    ao & 1u << 3 ? " [fence error]" : "", ao & 1u << 5 ? " [bus error]" : "");
	}
	fence = adreno8_get(&adreno8_gmu, "ahb fence status", GMU_AHB_FENCE_STATUS);
	if (fence != 0) {
		printf("adreno8: %u: NOTE: the fence has someone caught: %#x\n", adreno8_step, fence);
	}
	power = adreno8_get(&adreno8_gmu, "power clock status", GMU_PWR_CLK_STATUS);
	adreno8_get(&adreno8_gmu, "always on counter, high", GMU_ALWAYS_ON_HI);
	adreno8_get(&adreno8_gmu, "always on counter, low", GMU_ALWAYS_ON_LO);
	if (adreno8_hfi_view_table != 0) {
		printf("adreno8: %u: the queues at the end: command %u/%u, response %u/%u, debug %u/%u\n", adreno8_step,
		    adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(0) + 10) * 4), adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(0) + 11) * 4),
		    adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(1) + 10) * 4), adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(1) + 11) * 4), adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(2) + 10) * 4),
		    adreno8_rd(adreno8_hfi_view_table + (ADRENO8_HFI_HEADER(2) + 11) * 4));
	}
	printf("adreno8: %u: GMU diagnostics complete; log %#x, GX power/clock status %#x\n",
	    adreno8_step, adreno8_gmu_log.address, power);
	return true;
}

/* Ordinary HFI reaches START without HWSCHED features or host resource replies.
 * Firmware state and all mappings remain retained until cold reset after an attempt. */
bool
adreno8_ordinary_start(void)
{
	const uint32_t ifpc[] = {9, 0, 0}, handle = 0;

	if (!adreno8_direct_ring || adreno8_ordinary_attempted || adreno8_step != 14 ||
	    adreno8_newest_step != 17 || !adreno8_cx_ready() || !adreno8_gmu_talks ||
	    !adreno8_hfi_tables_sent || !adreno8_gpu8_keep || adreno8_gpu8_return) {
		return adreno8_hfi_failed();
	}
	/* The caller serializes startup; attempted closes reentry after success or failure. */
	adreno8_reset_required = true;
	if (!adreno8_hfi_send(11, ifpc, 3, "disable ordinary HFI IFPC") ||
	    !adreno8_hfi_send(14, &handle, 1, "ordinary core firmware") ||
	    !adreno8_hfi_send(10, NULL, 0, "ordinary START") ||
	    !adreno8_clock_started()) {
		return adreno8_hfi_failed();
	}
	adreno8_ordinary_attempted = true;
	adreno8_ordinary_started = true;
	printf("adreno8: ordinary HFI START acknowledged; direct ring initialization is pending\n");
	return true;
}

bool
adreno8_ordinary_ready(void)
{
	return adreno8_direct_ring && adreno8_ordinary_started;
}
