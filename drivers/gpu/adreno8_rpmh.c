/*
 * Apps DRV2 ACTIVE TCS1 ARC transactions: fixed MiCode rpmh-rsc.c v3 layout and trigger order,
 * https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/f66f8c8bcbe38044fab61d44ea7f7c224f82b3ea/drivers/soc/qcom/rpmh-rsc.c
 * plus upstream read protocol from edbafe65eef2b58625db1e113fbbfb1fe10c0291:
 * https://git.kernel.org/linus/edbafe65eef2b58625db1e113fbbfb1fe10c0291
 * Read requests use RESP_REQ, omit WRITE and CMD_DATA, and return CMD_RESP_DATA.
 * ACTIVE writes use WRITE | RESP_REQ, CMD_DATA and per-command completion wait.
 * RSC v3.8 uses an eight-byte message; the four-byte change applies from v4.5.
 */
#include "adreno8_rpmh_private.h"

#define printf adreno8_say
#define ARC_IRQ_ENABLE 0xd00u
#define ARC_IRQ_STATUS 0xd04u
#define ARC_IRQ_CLEAR 0xd08u
#define ARC_WAIT 0xfc0u
#define ARC_CONTROL 0xfc4u
#define ARC_STATUS 0xfc8u
#define ARC_ENABLE 0xfccu
#define ARC_MSGID 0xfd4u
#define ARC_ADDRESS 0xfd8u
#define ARC_DATA 0xfdcu
#define ARC_COMMAND_STATUS 0xfe0u
#define ARC_RESPONSE 0xfe4u
#define ARC_TCS_BIT (1u << 1)
#define ARC_AMC_ENABLE (1u << 16)
#define ARC_AMC_TRIGGER (1u << 24)
#define ARC_COMMAND_DONE ((1u << 8) | (1u << 16))

static struct adreno8_block arc_rsc = { "apps-rsc-drv2", 0x16520000, 0x10000 };
/* A completed read diagnostic permits one later clock preparation. Starting
 * either operation closes admission first; failures keep that state. */
static enum { ARC_COLD, ARC_READ_COMPLETE, ARC_CLOSED } arc_state;

struct arc_vote {
	uint32_t address, selector;
	unsigned count;
	uint16_t levels[RPMH_LEVELS];
};

static uint32_t
arc_read(const char *name, uint32_t offset)
{
	uint32_t value = adreno8_get(&arc_rsc, name, offset);
	adreno8_settle();
	return value;
}

static bool
arc_write(const char *name, uint32_t offset, uint32_t value)
{
	adreno8_settle();
	adreno8_put(&arc_rsc, name, offset, value);
	adreno8_settle();
	if (arc_read(name, offset) == value)
		return true;
	printf("adreno8: ARC register verification failed at %#x; retain TCS1 until reset\n", offset);
	return false;
}

static bool
arc_identity(void)
{
	uint32_t id = arc_read("id", 0);
	uint32_t solver = arc_read("solver config", 4);
	uint32_t config = arc_read("parent child config", 0xc);
	return ((id >> 8) & 0xffff) == 0x308 && solver != 0xffffffffu &&
	    (solver & (1u << 24)) == 0 && config != 0xffffffffu &&
	    ((config >> 12) & 0x3f) == 8 && (config >> 27) == 16;
}

static bool
arc_transfer(struct arc_vote *vote, bool write)
{
	uint32_t address = vote->address, control, irq_before, irq, command = 0, response;
	if (!arc_identity() || (write && vote->selector >= vote->count)) return false;
	control = arc_read("TCS1 control", ARC_CONTROL);
	irq_before = arc_read("irq enable", ARC_IRQ_ENABLE);
	if (irq_before == 0xffffffffu || (control & ~ARC_AMC_ENABLE) != 0 ||
	    arc_read("TCS0 status", 0xd28) != 1 || arc_read("TCS1 status", 0xfc8) != 1 ||
	    arc_read("TCS2 status", 0x1268) != 1 ||
	    arc_read("TCS1 enable", ARC_ENABLE) != 0 || arc_read("TCS1 wait", ARC_WAIT) != 0 ||
	    (arc_read("irq status", ARC_IRQ_STATUS) & ARC_TCS_BIT) != 0) {
		printf("adreno8: ARC transaction prerequisites failed; no registers written\n");
		return false;
	}

	/* A failed write or an unanswered request has no verified cancellation path.
	 * The reset requirement precedes the first write and remains set after success. */
	adreno8_reset_required = true;
	if (!arc_write("TCS1 clear trigger", ARC_CONTROL, control & ~ARC_AMC_TRIGGER) ||
	    !arc_write("TCS1 clear enable", ARC_CONTROL, 0) ||
	    !arc_write("TCS1 ARC message", ARC_MSGID, write ? 0x10108u : 0x108u) ||
	    !arc_write("TCS1 ARC address", ARC_ADDRESS, address))
		return false;
	if (write && (!arc_write("TCS1 ARC selector", ARC_DATA, vote->selector) ||
	    !arc_write("TCS1 ARC wait", ARC_WAIT, 1)))
		return false;
	if (!arc_write("TCS1 command enable", ARC_ENABLE, 1))
		return false;
	irq = arc_read("irq enable before request", ARC_IRQ_ENABLE);
	if (irq != irq_before || !arc_write("irq enable TCS1", ARC_IRQ_ENABLE, irq | ARC_TCS_BIT) ||
	    !arc_write("TCS1 AMC enable", ARC_CONTROL, ARC_AMC_ENABLE))
		return false;
	adreno8_settle();
	adreno8_put(&arc_rsc, "TCS1 ARC request trigger", ARC_CONTROL, ARC_AMC_ENABLE | ARC_AMC_TRIGGER);
	adreno8_settle();

	/* Match the upstream ten-second completion bound without per-poll log output. */
	for (unsigned poll = 0; poll < 100000; poll++) {
		irq = adreno8_rd(arc_rsc.base + ARC_IRQ_STATUS);
		adreno8_settle();
		if (irq == 0xffffffffu) {
			printf("adreno8: ARC poll returned an unreadable IRQ status; retain TCS1 until reset\n");
			return false;
		}
		if (irq & ARC_TCS_BIT)
			break;
		adreno8_sleep_us(100);
	}
	irq = arc_read("irq status after request", ARC_IRQ_STATUS);
	if (irq != 0xffffffffu && (irq & ARC_TCS_BIT))
		command = arc_read("TCS1 command status", ARC_COMMAND_STATUS);
	if (irq == 0xffffffffu || !(irq & ARC_TCS_BIT) || command == 0xffffffffu ||
	    (command & ARC_COMMAND_DONE) != ARC_COMMAND_DONE ||
	    arc_read("TCS1 completion status", ARC_STATUS) != 1) {
		printf("adreno8: ARC transaction completion unconfirmed; retain TCS1 and IRQ state until reset\n");
		return false;
	}
	if (!write) {
		response = arc_read("TCS1 ARC response", ARC_RESPONSE);
		if (response >= vote->count) {
			printf("adreno8: ARC response %#x exceeds %u CMD DB levels; retain TCS1 until reset\n",
			    response, vote->count);
			return false;
		}
		vote->selector = response;
	}
	irq = arc_read("TCS1 control after completion", ARC_CONTROL);
	if ((irq & ~(ARC_AMC_ENABLE | ARC_AMC_TRIGGER)) != 0 ||
	    !arc_write("TCS1 completed clear trigger", ARC_CONTROL, irq & ~ARC_AMC_TRIGGER) ||
	    !arc_write("TCS1 completed clear enable", ARC_CONTROL, 0) ||
	    !arc_write("TCS1 completed command disable", ARC_ENABLE, 0))
		return false;
	if (write && !arc_write("TCS1 completed clear wait", ARC_WAIT, 0))
		return false;
	adreno8_settle();
	adreno8_put(&arc_rsc, "TCS1 completed IRQ clear", ARC_IRQ_CLEAR, ARC_TCS_BIT);
	adreno8_settle();
	if ((arc_read("irq status after clear", ARC_IRQ_STATUS) & ARC_TCS_BIT) != 0 ||
	    !arc_write("TCS1 restore initial control", ARC_CONTROL, control))
		return false;
	irq = arc_read("irq enable before restore", ARC_IRQ_ENABLE);
	if (irq == 0xffffffffu ||
	    !arc_write("irq restore TCS1 bit", ARC_IRQ_ENABLE, irq & ~(ARC_TCS_BIT & ~irq_before)))
		return false;
	return true;
}

static bool
arc_resource_read(const char *resource, struct arc_vote *vote)
{
	vote->count = 0;
	vote->address = adreno8_rpmh_lookup(resource, vote->levels, &vote->count, NULL, NULL);
	if ((vote->address >> 16) != 3 || (vote->address & 3) != 0 ||
	    vote->count == 0 || vote->count > RPMH_LEVELS) {
		printf("adreno8: ARC read has no valid %s address and level table\n", resource);
		return false;
	}
	if (!arc_transfer(vote, false)) return false;
	printf("adreno8: ARC %s response selector %u, level %u; read completed, host cold reset required\n",
	    resource, vote->selector, vote->levels[vote->selector]);
	return true;
}

bool
adreno8_rpmh_arc_read(void)
{
	uint32_t asked = 0, touch = 0;
	struct arc_vote vote;

	PE_parse_boot_argn("adreno8_arc_read", &asked, sizeof(asked));
	if (asked == 0)
		return true;
	if (asked != 1 || arc_state != ARC_COLD) {
		printf("adreno8: ARC read requires value 1 and permits one attempt per kernel\n");
		return false;
	}
	arc_state = ARC_CLOSED;
	if (!PE_parse_boot_argn("touchscreen", &touch, sizeof(touch)) || touch != 0) {
		printf("adreno8: ARC read requires explicit touchscreen=0 to exclude the other DRV2 writer\n");
		return false;
	}
	/* Reuse TCS1 only after the preceding read has completed and restored it. */
	if (!arc_resource_read("cx.lvl", &vote) || !arc_resource_read("mx.lvl", &vote))
		return false;
	arc_state = ARC_READ_COMPLETE;
	return true;
}

bool
adreno8_rpmh_clock_prepare(void)
{
	struct arc_vote votes[2];
	uint32_t zero = 1, required[2];
	const char *names[] = {"cx.lvl", "mx.lvl"};
	const char *arguments[] = {"touchscreen", "wfi", "wfe_mode"};

	if (adreno8_gmu_from_pll == 0) return true;
	if (adreno8_gmu_from_pll > 2 || arc_state == ARC_CLOSED) return false;
	bool highest = adreno8_gmu_from_pll == 2, read_used = arc_state != ARC_COLD;
	arc_state = ARC_CLOSED;
	if (highest && (adreno8_step != 5 ||
	    (adreno8_newest_step != 5 && adreno8_newest_step < 17) || read_used)) return false;
	for (unsigned i = 0; i < 3; i++) {
		if (!PE_parse_boot_argn(arguments[i], &zero, sizeof(zero)) || zero != 0) {
			printf("adreno8: ARC clock votes require explicit %s=0\n", arguments[i]);
			return false;
		}
	}
	if (!arc_identity()) return false;
	/* The diagnostic has no sleep/wake cache or CPU_PM transition. Keep the
	 * observed sleep/wake/fast TCSes empty; only active AMC requests are issued. */
	for (unsigned i = 3; i < 8; i++) {
		uint32_t base = 0xd00u + i * 0x2a0u;
		if (arc_read("inactive TCS wait", base + 0x20) != 0 ||
		    arc_read("inactive TCS control", base + 0x24) != 0 ||
		    arc_read("inactive TCS status", base + 0x28) != 1 ||
		    arc_read("inactive TCS enable", base + 0x2c) != 0) return false;
	}
	if (highest) {
		/* The live Sun DT allows ARC LEVEL_MAX on CX/MX. Validate the complete
		 * observed CMD DB maps before selecting their highest legal corners.
		 * rpmhpd.c uses level_count-1 before sync_state; rpmh_write completes
		 * on the response IRQ without a resource READ. This stage-5 diagnostic
		 * keeps that completion contract and leaves physical rail state unmeasured. */
		static const uint16_t expected[] = {0, 16, 64, 128, 192, 256, 320, 384, 416, 448, 480};
		for (unsigned i = 0; i < 2; i++) {
			const uint8_t *data = NULL;
			unsigned length = 0, count = i == 0 ? 11 : 10;
			votes[i].address = adreno8_rpmh_lookup(names[i], votes[i].levels, &votes[i].count, &data, &length);
			if (votes[i].address != 0x30000u + i * 0x10u || votes[i].count != count ||
			    !data || length < 2 * count || length > 32 || (length & 1) ||
			    memcmp(votes[i].levels, expected, count * sizeof(expected[0])) != 0) {
				printf("adreno8: ARC highest-corner diagnostic has an unknown %s CMD DB map\n", names[i]);
				return false;
			}
			for (unsigned j = 0; j < length / 2; j++) {
				uint16_t level = (uint16_t)(data[2 * j] | data[2 * j + 1] << 8);
				if (level != (j < count ? expected[j] : 0)) return false;
			}
			votes[i].selector = count - 1;
		}
		for (unsigned i = 0; i < 2; i++) {
			if (!arc_transfer(&votes[i], true)) return false;
			printf("adreno8: ARC %s highest legal selector %u level %u WRITE completed; retained until cold reset\n",
			    names[i], votes[i].selector, votes[i].levels[votes[i].selector]);
		}
		return true;
	}
	/* gpucc-sun.c uses VDD_LOW (level 128) on both rails for GMU 650 MHz.
	 * Keep any higher inherited DRV2 vote, including after the switch to 500 MHz. */
	for (unsigned i = 0; i < 2; i++) {
		if (!arc_resource_read(names[i], &votes[i]) || votes[i].levels[0] != 0)
			return false;
		required[i] = votes[i].count;
		for (unsigned j = 1; j < votes[i].count; j++) {
			if (votes[i].levels[j] <= votes[i].levels[j - 1]) return false;
			if (required[i] == votes[i].count && votes[i].levels[j] >= 128) required[i] = j;
		}
		if (required[i] == votes[i].count) return false;
		if (required[i] < votes[i].selector) required[i] = votes[i].selector;
	}
	if (votes[0].address == votes[1].address) return false;
	for (unsigned i = 0; i < 2; i++) {
		if (votes[i].selector != required[i]) {
			votes[i].selector = required[i];
			if (!arc_transfer(&votes[i], true) || !arc_transfer(&votes[i], false) ||
			    votes[i].selector != required[i]) return false;
		}
		printf("adreno8: ARC %s ACTIVE selector %u level %u held until cold reset\n",
		    names[i], votes[i].selector, votes[i].levels[votes[i].selector]);
	}
	return true;
}
