/* Sun GPUCC TAYCAN_ELU PLL and RCG2 sequence, from MiCode kernel revision
 * f66f8c8bcbe38044fab61d44ea7f7c224f82b3ea: gpucc-sun.c, gcc-sun.c,
 * clk-alpha-pll.c and clk-rcg2.c. The host keeps CX/MX votes until cold reset. */
#include "adreno8_clock_private.h"
#include "adreno8_rpmh_private.h"

#define printf adreno8_say
#define PLL_MODE 0x00u
#define PLL_OPMODE 0x04u
#define PLL_L 0x10u
#define PLL_ALPHA 0x14u
#define PLL_USER 0x18u
#define PLL_OUT (1u << 0)
#define PLL_RESET_N (1u << 2)
#define PLL_PCAL_NOT_DONE (1u << 8)
#define PLL_ACK (1u << 13)
#define PLL_LATCH (1u << 14)
#define PLL_VOTE_RUN (1u << 25)
#define PLL_LOCK (1u << 31)
#define RCG_ROOT_EN (1u << 1)
#define RCG_ROOT_OFF (1u << 31)
#define RCG_CONFIG_MASK 0x10371fu
#define RCG_GMU_CONFIG 0x100201u
#define RCG_HUB_CONFIG 0x100505u
#define GCC_PLL0_VOTE 0x52020u
#define GCC_GPU_PLL0_GATE 0x52000u
#define GCC_GPU_PLL0_ENABLE (1u << 15)

static enum { CLOCK_COLD, CLOCK_BOOT, CLOCK_RUNNING, CLOCK_FAILED } clock_state;

static bool
clock_fail(const char *what)
{
	clock_state = CLOCK_FAILED;
	adreno8_reset_required = true;
	printf("adreno8: GMU/HUB clock %s failed; clock path closed until cold reset\n", what);
	return false;
}

static bool
clock_write(struct adreno8_block *block, const char *name, uint32_t offset,
    uint32_t mask, uint32_t value)
{
	uint32_t was = adreno8_get(block, name, offset);
	if (was == 0xffffffffu) return false;
	adreno8_put(block, name, offset, (was & ~mask) | value);
	adreno8_settle();
	uint32_t read = adreno8_get(block, name, offset);
	return read != 0xffffffffu && (read & mask) == value;
}

static bool
clock_matches(struct adreno8_block *block, const char *name, uint32_t offset,
    uint32_t mask, uint32_t expected)
{
	uint32_t value = adreno8_get(block, name, offset);
	return value != 0xffffffffu && (value & mask) == expected;
}

static bool
clock_poll(struct adreno8_block *block, uint32_t offset, uint32_t mask,
    uint32_t expected, unsigned microseconds)
{
	for (unsigned i = 0; i < microseconds; i++) {
		uint32_t value = adreno8_rd(block->base + offset);
		if (value == 0xffffffffu) return false;
		if ((value & mask) == expected) return true;
		adreno8_sleep_us(1);
	}
	return false;
}

static bool
pll_frequency(struct adreno8_block *block, uint32_t expected)
{
	uint32_t mode = adreno8_get(block, "PLL0 mode", PLL_MODE);
	uint32_t l = adreno8_get(block, "PLL0 L", PLL_L);
	uint32_t alpha = adreno8_get(block, "PLL0 alpha", PLL_ALPHA);
	if (mode == 0xffffffffu || !(mode & PLL_LOCK) || l == 0xffffffffu || alpha > 0xffff)
		return false;
	/* The vendor's 16-bit-alpha rate calculation rounds upward to whole kHz. */
	uint64_t hz = 19200000ULL * (l & 0xffff) + ((19200000ULL * alpha) >> 16);
	return (hz + 999) / 1000 * 1000 == expected;
}

static bool
pll_configure(void)
{
	uint32_t user = adreno8_get(&adreno8_gpucc, "PLL0 user", PLL_USER);
	uint32_t mode = adreno8_get(&adreno8_gpucc, "PLL0 mode", PLL_MODE);
	uint32_t opmode = adreno8_get(&adreno8_gpucc, "PLL0 opmode", PLL_OPMODE);
	if (user == 0xffffffffu || mode == 0xffffffffu || opmode == 0xffffffffu ||
	    (user & PLL_VOTE_RUN)) return false; /* GPU PLL0 has no host FSM vote register. */
	uint32_t gmu_source = adreno8_get(&adreno8_gpucc, "inherited GMU source", GPUCC_GMU_RCGR + 4);
	uint32_t hub_source = adreno8_get(&adreno8_gpucc, "inherited HUB source", GPUCC_HUB_RCGR + 4);
	if (gmu_source == 0xffffffffu || hub_source == 0xffffffffu) return false;
	uint32_t gmu_parent = (gmu_source >> 8) & 7, hub_parent = (hub_source >> 8) & 7;
	/* PLL0 changes before either RCG is reconfigured. Its only accepted inherited
	 * consumer is GMU parent2 already divided by two. HUB starts at XO or 200 MHz. */
	if (gmu_parent != 0 && gmu_parent != 2 && gmu_parent != 5 && gmu_parent != 6) return false;
	if (hub_parent != 0 && (hub_source & 0x371f) != 0x505) return false;
	if (gmu_parent == 2 && (user & 0x3c00) != 0x400) return false;
	bool enabled = (opmode & 1) && (mode & PLL_OUT);
	if (!clock_write(&adreno8_gpucc, "PLL0 outputs", PLL_USER, 7, 7)) return false;
	if (enabled) return true;
	if (!clock_write(&adreno8_gpucc, "PLL0 reset", PLL_MODE, PLL_RESET_N, 0)) return false;
	uint32_t l = adreno8_get(&adreno8_gpucc, "PLL0 L", PLL_L);
	if (l == 0xffffffffu) return false;
	if (l & 0xffff) return true; /* Preserve an initialized loader configuration. */
	static const struct { uint32_t offset, value; } config[] = {
		{PLL_L, 0x44440034}, {PLL_ALPHA, 0x1555},
		{0x20, 0x19660387}, {0x24, 0x098060a0}, {0x28, 0xb416cb20},
		{PLL_USER, 0x407}, {0x1c, 2}, {0x2c, 0}, {0x30, 0},
	};
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		if (config[i].offset == PLL_L) {
			/* Annibale reads L[31:24] as zero after this vendor write. Keep the
			 * full write; rate and calibration calculations use the low 24 bits. */
			adreno8_put(&adreno8_gpucc, "PLL0 configure L", PLL_L, config[i].value);
			adreno8_settle();
			if (!clock_matches(&adreno8_gpucc, "PLL0 configure L", PLL_L,
			    0x00ffffffu, 0x00440034u)) return false;
		} else if (!clock_write(&adreno8_gpucc, "PLL0 configure", config[i].offset,
		    0xffffffffu, config[i].value)) return false;
	}
	return clock_write(&adreno8_gpucc, "PLL0 output off", PLL_MODE, PLL_OUT, 0);
}

static bool
pll_enable(void)
{
	uint32_t mode = adreno8_get(&adreno8_gpucc, "PLL0 mode", PLL_MODE);
	uint32_t opmode = adreno8_get(&adreno8_gpucc, "PLL0 opmode", PLL_OPMODE);
	if (mode == 0xffffffffu || opmode == 0xffffffffu) return false;
	if (!((mode & PLL_OUT) && (opmode & 1))) {
		if (!clock_write(&adreno8_gpucc, "PLL0 reset release", PLL_MODE, PLL_RESET_N, PLL_RESET_N) ||
		    !clock_write(&adreno8_gpucc, "PLL0 run", PLL_OPMODE, 0xffffffffu, 1)) return false;
	}
	if (!clock_poll(&adreno8_gpucc, PLL_MODE, PLL_LOCK, PLL_LOCK, 1500)) return false;
	return clock_write(&adreno8_gpucc, "PLL0 output", PLL_MODE, PLL_OUT, PLL_OUT);
}

static bool
pll_rate(uint32_t l, uint32_t alpha, uint32_t hz)
{
	if (!clock_write(&adreno8_gpucc, "PLL0 L rate", PLL_L, 0xffff, l) ||
	    !clock_write(&adreno8_gpucc, "PLL0 alpha rate", PLL_ALPHA, 0xffffffffu, alpha)) return false;
	uint32_t mode = adreno8_get(&adreno8_gpucc, "PLL0 latch mode", PLL_MODE);
	if (mode == 0xffffffffu) return false;
	/* The latch interface is disabled in reset. RUN and STANDBY require its ACK. */
	if (mode & PLL_RESET_N) {
		if (!clock_write(&adreno8_gpucc, "PLL0 latch input", PLL_MODE, PLL_LATCH, PLL_LATCH)) return false;
		adreno8_sleep_us(1);
		uint32_t ack = adreno8_get(&adreno8_gpucc, "PLL0 latch ACK", PLL_MODE);
		if (ack == 0xffffffffu || !(ack & PLL_ACK) ||
		    !clock_write(&adreno8_gpucc, "PLL0 latch release", PLL_MODE, PLL_LATCH, 0)) return false;
	}
	uint32_t opmode = adreno8_get(&adreno8_gpucc, "PLL0 rate opmode", PLL_OPMODE);
	if (opmode == 0xffffffffu) return false;
	if ((mode & PLL_OUT) && (opmode & 1) &&
	    !clock_poll(&adreno8_gpucc, PLL_MODE, PLL_LOCK, PLL_LOCK, 1500)) return false;
	adreno8_sleep_us(100);
	/* A disabled PLL is checked after enable, once LOCK can be asserted. */
	return !((mode & PLL_OUT) && (opmode & 1)) || pll_frequency(&adreno8_gpucc, hz);
}

static bool
hub_parent(void)
{
	adreno8_need(&adreno8_gcc);
	uint32_t user = adreno8_get(&adreno8_gcc, "GPLL0 user", PLL_USER);
	if (user == 0xffffffffu || !(user & PLL_VOTE_RUN)) return false;
	/* GPLL0 is a shared fixed-rate clock. Only its HLOS vote and GPU output gate
	 * are writable here; its L/alpha and other consumers' votes remain unchanged. */
	if (!clock_write(&adreno8_gcc, "GPLL0 vote", GCC_PLL0_VOTE, 1, 1)) return false;
	if (!clock_poll(&adreno8_gcc, PLL_MODE, PLL_LOCK, PLL_LOCK, 1500) ||
	    !pll_frequency(&adreno8_gcc, 600000000) ||
	    !clock_write(&adreno8_gcc, "GPU GPLL0 gate", GCC_GPU_PLL0_GATE,
	    GCC_GPU_PLL0_ENABLE, GCC_GPU_PLL0_ENABLE)) return false;
	adreno8_sleep_us(10); /* gcc_gpu_gpll0_clk_src uses BRANCH_HALT_DELAY. */
	return true;
}

static bool
rcg_configure(uint32_t command, uint32_t config)
{
	uint32_t saved = adreno8_get(&adreno8_gpucc, "RCG command", command);
	if (saved == 0xffffffffu ||
	    !clock_write(&adreno8_gpucc, "RCG force enable", command, RCG_ROOT_EN, RCG_ROOT_EN) ||
	    !clock_poll(&adreno8_gpucc, command, RCG_ROOT_OFF, 0, 500) ||
	    !clock_write(&adreno8_gpucc, "RCG config", command + 4, RCG_CONFIG_MASK, config)) return false;
	/* UPDATE can clear before the first read; its completion is the acknowledgement. */
	uint32_t value = adreno8_get(&adreno8_gpucc, "RCG update command", command);
	if (value == 0xffffffffu) return false;
	adreno8_put(&adreno8_gpucc, "RCG update", command, value | RCG_UPDATE);
	adreno8_settle();
	if (!clock_poll(&adreno8_gpucc, command, RCG_UPDATE, 0, 500)) return false;
	return clock_write(&adreno8_gpucc, "RCG force restore", command, RCG_ROOT_EN, saved & RCG_ROOT_EN);
}

static bool
clock_verify(uint32_t pll_hz)
{
	return pll_frequency(&adreno8_gpucc, pll_hz) &&
	    pll_frequency(&adreno8_gcc, 600000000) &&
	    clock_matches(&adreno8_gpucc, "PLL0 running mode", PLL_MODE, PLL_LOCK | PLL_RESET_N | PLL_OUT,
	    PLL_LOCK | PLL_RESET_N | PLL_OUT) &&
	    clock_matches(&adreno8_gpucc, "PLL0 running opmode", PLL_OPMODE, 1, 1) &&
	    clock_matches(&adreno8_gpucc, "PLL0 even divider", PLL_USER, 0x3c00, 0x400) &&
	    clock_matches(&adreno8_gpucc, "GMU clock config", GPUCC_GMU_RCGR + 4, RCG_CONFIG_MASK, RCG_GMU_CONFIG) &&
	    clock_matches(&adreno8_gpucc, "HUB clock config", GPUCC_HUB_RCGR + 4, RCG_CONFIG_MASK, RCG_HUB_CONFIG) &&
	    clock_matches(&adreno8_gcc, "GPU GPLL0 gate", GCC_GPU_PLL0_GATE, GCC_GPU_PLL0_ENABLE, GCC_GPU_PLL0_ENABLE);
}

bool
adreno8_clock_boot(void)
{
	if (!adreno8_gmu_from_pll) return true;
	if (adreno8_gmu_from_pll == 2 && (adreno8_step != 5 ||
	    (adreno8_newest_step != 5 && adreno8_newest_step < 17)))
		return clock_fail("short clock diagnostic admission");
	if (clock_state == CLOCK_FAILED) return false;
	if (clock_state != CLOCK_COLD)
		return clock_verify(clock_state == CLOCK_BOOT ? 1300000000 : 1000000000) || clock_fail("repeat verification");
	if (!adreno8_rpmh_clock_prepare()) return clock_fail("resource votes");
	adreno8_reset_required = true;
	if (!pll_configure() || !hub_parent() || !pll_rate(0x43, 0xb555, 1300000000) ||
	    !clock_write(&adreno8_gpucc, "PLL0 even divider", PLL_USER, 0x3c00, 0x400))
		return clock_fail("parent configuration");
	uint32_t calibration = adreno8_get(&adreno8_gpucc, "PLL0 calibration mode", PLL_MODE);
	if (calibration == 0xffffffffu) return clock_fail("calibration read");
	if (calibration & PLL_PCAL_NOT_DONE) {
		/* The retained resource vote covers the fixed Sun calibration value.
		 * The vendor prepare path derives a separate voltage for other values. */
		uint32_t l = adreno8_get(&adreno8_gpucc, "PLL0 calibration L", PLL_L);
		if ((l & 0x00ff0000u) != 0x00440000u) return clock_fail("inherited calibration fields");
		if (!pll_enable() ||
		    !clock_write(&adreno8_gpucc, "PLL0 calibration output off", PLL_MODE, PLL_OUT, 0) ||
		    !clock_write(&adreno8_gpucc, "PLL0 calibration standby", PLL_OPMODE, 0xffffffffu, 0) ||
		    !clock_write(&adreno8_gpucc, "PLL0 calibration reset", PLL_MODE, PLL_RESET_N, 0))
			return clock_fail("calibration");
	}
	if (!pll_enable() || !pll_frequency(&adreno8_gpucc, 1300000000) ||
	    !rcg_configure(GPUCC_GMU_RCGR, RCG_GMU_CONFIG) ||
	    !rcg_configure(GPUCC_HUB_RCGR, RCG_HUB_CONFIG) || !clock_verify(1300000000))
		return clock_fail("650/200 MHz startup");
	uint32_t hub_divider = adreno8_get(&adreno8_gpucc, "HUB read-only divider", GPUCC_HUB_DIV);
	if (hub_divider == 0xffffffffu) return clock_fail("HUB divider read");
	clock_state = CLOCK_BOOT;
	printf("adreno8: GMU 650 MHz / HUB 200 MHz clock configuration and parent LOCK read back; HUB divider %#x\n",
	    hub_divider);
	return true;
}

bool
adreno8_clock_started(void)
{
	if (!adreno8_gmu_from_pll) return true;
	if (adreno8_gmu_from_pll != 1 && !(adreno8_gmu_from_pll == 2 &&
	    adreno8_newest_step >= 17 && adreno8_step == 14))
		return clock_fail("short clock diagnostic START admission");
	if (clock_state == CLOCK_RUNNING) return clock_verify(1000000000) || clock_fail("running verification");
	if (clock_state != CLOCK_BOOT) return clock_fail("START order");
	if (!pll_rate(0x34, 0x1555, 1000000000) || !clock_verify(1000000000))
		return clock_fail("650 to 500 MHz transition");
	clock_state = CLOCK_RUNNING;
	printf("adreno8: GMU 500 MHz after START; HUB 200 MHz and host resource votes retained\n");
	return true;
}
