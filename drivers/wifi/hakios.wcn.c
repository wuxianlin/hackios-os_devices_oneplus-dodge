/*
 * hakios.wcn.c — the WiFi radio of the OnePlus 13 (dodge, SM8750): a WCN7850
 * (ath12k in Linux) on PCIe0, brought up in stages behind the boot-arg
 * wifi_pcie=N (the WCN7850's own driver, MHI and the 802.11 stack come after
 * the link does).
 *
 * What each stage touches, in order (the references: UniXNU-sheng's
 * WifiProbe.cpp for the sequence, our device tree for the numbers, gcc-sun.c
 * of the OnePlus kernel for the clock branches):
 *
 *   1  read only: the WLAN_EN/PERST/WAKE pins' TLMM state, the two GDSCs'
 *      power bits, the PCIe0 clock branches' halt bits — what the boot
 *      loaders left, before anything is asked for
 *   2  WLAN_EN high (TLMM gpio16), the PCIe0 GDSC and the PHY GDSC on
 *   3  the PCIe0 clock branches on (slv/mstr/cfg/aux/pipe/slv_q2a/phy_rchng)
 *   4  the PHY: the device tree's own init table (qcom,phy-sequence, PHY v5e)
 *      and the power-down release, polled to PHYSTATUS
 *   5  the PARF and the soft-macro sequencer (the qcom,pcie-sm-seq program)
 *   6  PERST released, the LTSSM started, the link polled up
 *   7  the endpoint's config space: vendor and device id (17cb:1107 is the
 *      WCN7850), the BARs, the MSI table
 *
 * Numbers (device tree and gcc-sun.c):
 *   TLMM          0x0f100000 + 0x1000 * pin (per-pin bank; the touch's proof)
 *   gcc           0x100000 + 0x52008 (bits 0..5) and + 0x52000 (bit 22)
 *   GDSCs         0x16b004 (PCIe0) and 0x16c000 (the PHY's)
 *   the PHY       0x1c06000, power-down +0x440, status +0x414 bit 6
 *   PARF          0x1c00000
 *   the soft mac  0x40100000
 */

#include <mach/mach_types.h>
#include <sys/kernel_types.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay_for_interval(uint32_t interval, uint64_t unit);

#define WIFI_TLMM_PA    0x0f100000ULL     /* the per-pin bank (touch-proven) */
#define WIFI_GCC_PA     0x01000000ULL
#define WIFI_GDSC_PA    0x016b004ULL      /* PCIe0's; the PHY's is +0xffc (0x16c000) */
#define WIFI_PHY_PA     0x01c06000ULL
#define WIFI_PARF_PA    0x01c00000ULL

/* the three pins (device tree: cnss_wlan_en gpio16, perst-gpio 102, wake 104) */
#define PIN_WLAN_EN     16
#define PIN_PERST       102
#define PIN_WAKE        104

/* gcc clock-branch votes (gcc-sun.c): write the enable bit, poll the halt
 * register's bit 31 (CLK_OFF) clear */
static const struct {
	const char *name;
	uint32_t vote_off, bit, halt_off;
} branches[] = {
	{ "slv_axi",   0x52008, 0,  0x6b020 },
	{ "mstr_axi",  0x52008, 1,  0x6b030 },
	{ "cfg_ahb",   0x52008, 2,  0x6b040 },
	{ "aux",       0x52008, 3,  0x6b044 },
	{ "pipe",      0x52008, 4,  0x6b054 },
	{ "slv_q2a",   0x52008, 5,  0x6b01c },
	{ "phy_rchng", 0x52000, 22, 0x6b064 },
};

static uint32_t wifi_pcie_step;      /* wifi_pcie=N: run stages 1..N and stop */
static uint64_t rm_tx_capid;         /* this boot's Gunyah RM msgq caps (from the loader's DT) */
static uint64_t rm_rx_capid;

static volatile uint32_t *tlmm_map;
static volatile uint32_t *gcc_map;

/* One ARM SMCCC call from kernel context. kind 0 = SMC, 1 = HVC in the
 * SMCCC form (x0 carries the FID), 2 = HVC in Qualcomm's legacy form where
 * the function is the hvc IMMEDIATE (0x601B send / 0x601C recv, what the
 * vendor gh_msgq driver uses) and x0.. carry the arguments. Zero risk: the
 * firmware answers errors instead of hanging. */
static uint64_t
gh_call(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, int kind, uint64_t *r1, uint64_t *r2, uint64_t *r3)
{
	register uint64_t x0 __asm__("x0") = a1;
	register uint64_t x1 __asm__("x1") = a2;
	register uint64_t x2 __asm__("x2") = a3;
	register uint64_t x3 __asm__("x3") = a4;
	register uint64_t x4 __asm__("x4") = 0;

	if (kind == 2 && fid == 0x601Cu) {
		__asm__ volatile ("hvc #0x601C" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4) : : "memory");
	} else if (kind == 2 && fid == 0x601Bu) {
		__asm__ volatile ("hvc #0x601B" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4) : : "memory");
	} else if (kind == 1) {
		x0 = fid;
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4) : : "memory");
	} else {
		x0 = fid;
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4) : : "memory");
	}
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	if (r3 != NULL) {
		*r3 = x3;
	}
	return x0;
}

static inline uint32_t
rd(volatile uint32_t *a)
{
	return *a;
}

static inline void
wr(volatile uint32_t *a, uint32_t v)
{
	*a = v;
	__asm__ volatile ("dsb sy");
}

/* a pin's config word and its output value word (the touch driver's layout) */
static volatile uint32_t *
pin_cfg(unsigned pin)
{
	return (volatile uint32_t *)((uintptr_t)tlmm_map + 0x1000UL * pin);
}

static void
pin_out(unsigned pin, bool high)
{
	wr(pin_cfg(pin) + 1, high ? 2 : 0);
}

static void
pin_gpio(unsigned pin, bool output)
{
	uint32_t v = rd(pin_cfg(pin));

	wr(pin_cfg(pin), (v & ~0x3ffu) | (2u << 6) | ((uint32_t)output << 9));
}

/* ---- stage 1: read only -------------------------------------------------------------------------- */

static void
stage_inspect(void)
{
	printf("wifi: stage 1: what the boot loaders left\n");
	for (unsigned i = 0; i < 3; i++) {
		unsigned pin = i == 0 ? PIN_WLAN_EN : i == 1 ? PIN_PERST : PIN_WAKE;
		uint32_t cfg = rd(pin_cfg(pin));
		uint32_t val = rd(pin_cfg(pin) + 1);

		printf("wifi: TLMM pin %u: config %#010x (mux %u, out %u), in %u, value-word %#x\n",
		    pin, cfg, (cfg >> 6) & 7, (cfg >> 9) & 1, cfg & 1, val);
	}
	if (gcc_map != NULL && wifi_pcie_step >= 3) {
		/* the gcc block is Gunyah-filtered from this VM (both the vote page
		 * 0x1052008 and the GDSC page 0x16b004 fault, 2026-10-29); only
		 * asked for at wifi_pcie=3, where a panic is the expected answer
		 * until another way in is found */
		/* the GDSC page first: the two GDSCs (0x16b004 PCIe0, 0x16c000 the
		 * PHY's), bit 31 PWR_ON; a fault here costs a panic that returns */
		{
			uint32_t g0 = rd((volatile uint32_t *)((uintptr_t)gcc_map + 0x6b004));

			printf("wifi: GDSC PCIe0 %#010x (%s)\n", g0, (g0 >> 31) & 1 ? "power on" : "power off");
		}
		{
			uint32_t g1 = rd((volatile uint32_t *)((uintptr_t)gcc_map + 0x16c000 - 0x100000));

			printf("wifi: GDSC phy %#010x (%s)\n", g1, (g1 >> 31) & 1 ? "power on" : "power off");
		}
		/* the halt page next: each branch's CLK_OFF (bit 31) */
		for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
			uint32_t halt = rd((volatile uint32_t *)((uintptr_t)gcc_map + branches[i].halt_off));

			printf("wifi: %s: the branch %s (halt %#010x)\n", branches[i].name,
			    (halt >> 31) & 1 ? "halted (clock off)" : "running", halt);
		}
		/* the vote page last: it has faulted from this VM before
		 * (far gcc+0x52008, 2026-10-29) -- reading it is its own print */
		{
			uint32_t votes08 = rd((volatile uint32_t *)((uintptr_t)gcc_map + branches[0].vote_off));

			printf("wifi: gcc votes %#010x readable\n", votes08);
		}
	}
}

/* The DT's own bring-up program for this phone (pci-msm.c's CESTA):
 * qcom,pcie-sm-seq (13 words), qcom,pcie-sm-branch-seq (7), the branch
 * offset 0x1000 and the start offset 0x1090; the PHY's init table
 * qcom,phy-sequence ((offset, value, delay) triplets) with the status at
 * +0x414 bit 6 and the power-down at +0x440; the aux clock 19.2 MHz
 * (qcom,aux-clk-freq = 0x14).  All of it is in our device tree dump. */
static const uint32_t sm_seq[] = {
	0x1c018081, 0x70074002, 0x50028000, 0x28007003, 0x80804002, 0x70021c01,
	0x18001802, 0x70005000, 0x10004000, 0x80814002, 0x18001c01, 0x1c018080, 0x100,
};
static const uint32_t sm_branch_seq[] = {
	0x4, 0x1c, 0x24, 0x2c, 0x0, 0x0, 0x0,
};
#define SM_BRANCH_OFF  0x1000
#define SM_START_OFF   0x1090

/* the PHY init table (offset, value, delay-in-us triplets; the delay of most
 * is 0) -- the whole qcom,phy-sequence of the device tree, verbatim */
static const struct { uint16_t off; uint8_t val; uint8_t us; } phy_seq[] = {
	{0x440, 0x3, 0},
	{0xc0, 0x1, 0}, {0xcc, 0x62, 0}, {0xd0, 0x2, 0}, {0x60, 0xf8, 0}, {0x64, 0x1, 0},
	{0x0, 0x93, 0}, {0x4, 0x1, 0}, {0xe0, 0x90, 0}, {0xe4, 0x82, 0}, {0xf4, 0x7, 0},
	{0x70, 0x2, 0}, {0x10, 0x2, 0}, {0x74, 0x16, 0}, {0x14, 0x16, 0}, {0x78, 0x36, 0},
	{0x18, 0x36, 0}, {0x110, 0x8, 0}, {0xbc, 0xa, 0}, {0x120, 0x42, 0}, {0x80, 0x4, 0},
	{0x84, 0xd, 0}, {0x20, 0xa, 0}, {0x24, 0x1a, 0}, {0x88, 0x41, 0}, {0x28, 0x34, 0},
	{0x90, 0xab, 0}, {0x94, 0xaa, 0}, {0x98, 0x1, 0}, {0x30, 0x55, 0}, {0x34, 0x55, 0},
	{0x38, 0x1, 0}, {0x140, 0x14, 0}, {0x164, 0x34, 0}, {0x3c, 0x1, 0}, {0x1c, 0x4, 0},
	{0x174, 0x16, 0}, {0x1bc, 0xf, 0}, {0x170, 0xa0, 0},
	/* rx lane 1 (0x1000 block) */
	{0x13a4, 0x38, 0}, {0x12dc, 0x11, 0}, {0x1360, 0xbf, 0}, {0x1364, 0xbf, 0},
	{0x1368, 0xb7, 0}, {0x136c, 0xea, 0}, {0x135c, 0x3f, 0}, {0x1374, 0x9, 0},
	{0x1378, 0x49, 0}, {0x137c, 0x1b, 0}, {0x1380, 0x8f, 0}, {0x1370, 0xd1, 0},
	{0x1388, 0x9, 0}, {0x138c, 0x49, 0}, {0x1390, 0x1b, 0}, {0x1394, 0x8f, 0},
	{0x1384, 0xd1, 0}, {0x12c4, 0x3e, 0}, {0x12c8, 0x1e, 0}, {0x12cc, 0xd2, 0},
	{0x1208, 0x9, 0}, {0x1214, 0x5, 0}, {0x124c, 0x8, 0}, {0x1250, 0x8, 0},
	{0x12d8, 0x9, 0}, {0x1318, 0x1c, 0}, {0x131c, 0x60, 0}, {0x12f8, 0x7, 0},
	{0x13f8, 0x8, 0}, {0x1800, 0x0, 0},
	/* tx lane 1 (0x1000 block) */
	{0x1084, 0x35, 0}, {0x108c, 0x10, 0}, {0x1090, 0x31, 0}, {0x1094, 0x7f, 0},
	{0x10e4, 0x2, 0}, {0x1040, 0x8, 0}, {0x103c, 0x14, 0},
	/* rx lane 2 (0x1800/0x1a00) */
	{0x1ba4, 0x38, 0}, {0x1adc, 0x11, 0}, {0x1b60, 0xbf, 0}, {0x1b64, 0xbf, 0},
	{0x1b68, 0xb7, 0}, {0x1b6c, 0xea, 0}, {0x1b5c, 0x3f, 0}, {0x1b74, 0x9, 0},
	{0x1b78, 0x49, 0}, {0x1b7c, 0x1b, 0}, {0x1b80, 0x8f, 0}, {0x1b70, 0xd1, 0},
	{0x1b88, 0x9, 0}, {0x1b8c, 0x49, 0}, {0x1b90, 0x1b, 0}, {0x1b94, 0x8f, 0},
	{0x1b84, 0xd1, 0}, {0x1ac4, 0x3e, 0}, {0x1ac8, 0x1e, 0}, {0x1acc, 0xd2, 0},
	{0x1a08, 0x9, 0}, {0x1a14, 0x5, 0}, {0x1a4c, 0x8, 0}, {0x1a50, 0x8, 0},
	{0x1ad8, 0x9, 0}, {0x1b18, 0x1c, 0}, {0x1b1c, 0x60, 0}, {0x1af8, 0x7, 0},
	{0x1bf8, 0x8, 0},
	/* tx lane 2 */
	{0x1884, 0x35, 0}, {0x188c, 0x10, 0}, {0x1890, 0x31, 0}, {0x1894, 0x7f, 0},
	{0x18e4, 0x2, 0}, {0x1840, 0x8, 0}, {0x183c, 0x14, 0},
	/* pcs */
	{0x4dc, 0x5, 0}, {0x588, 0x77, 0}, {0x598, 0xb, 0}, {0x8a4, 0x1e, 0},
	{0x8f4, 0x27, 0}, {0x5e4, 0xf, 0}, {0x80c, 0x1d, 0}, {0x814, 0x7, 0},
	{0x820, 0xc1, 0}, {0x894, 0x0, 0}, {0x8f8, 0x1f, 0},
	/* pcs_misc */
	{0x5d0, 0x8c, 0}, {0x568, 0x17, 0}, {0x570, 0x2e, 0}, {0x400, 0x0, 0},
	{0x444, 0x3, 0},
};

static volatile uint32_t *sm_map;
static volatile uint32_t *parf_map;
static volatile uint32_t *phy_map;

/* ---- stage 2: CESTA -- the state manager's own program, the PARF, the PHY, the link ------------- */

static void
stage_cesta(void)
{
	printf("wifi: stage 2: the CESTA state manager, the PARF, the PHY, the link\n");

	/* WLAN_EN high and PERST asserted (low) first: the radio's power and
	 * its reset, both plain TLMM pins */
	pin_gpio(PIN_WLAN_EN, true);
	pin_out(PIN_WLAN_EN, true);
	pin_gpio(PIN_PERST, true);
	pin_out(PIN_PERST, false);
	printf("wifi: WLAN_EN %#010x, PERST %#010x\n", rd(pin_cfg(PIN_WLAN_EN)), rd(pin_cfg(PIN_PERST)));

	/* the CESTA program: the state manager sequences the clocks and the
	 * power through its own path (that is why gcc is not touched) */
	if (sm_map == NULL) {
		printf("wifi: no state-manager window\n");
		return;
	}
	/* the PWR_CTRL override: writing 1 to both it and PWR_MASK forces the
	 * power on for this instance (what the driver's module init does; the
	 * CESTA sequence replaces the override once it runs) */
	{
		uint32_t ctrl_off = 0x1040, mask_off = 0x1048, inst_off = 0x3000;

		wr((volatile uint32_t *)((uintptr_t)sm_map + ctrl_off), 1);
		wr((volatile uint32_t *)((uintptr_t)sm_map + mask_off), 1);
		printf("wifi: the power override set (ctrl %#010x, mask %#010x)\n",
		    rd((volatile uint32_t *)((uintptr_t)sm_map + ctrl_off)),
		    rd((volatile uint32_t *)((uintptr_t)sm_map + mask_off)));
	}
	for (unsigned i = 0; i < sizeof(sm_seq) / sizeof(sm_seq[0]); i++) {
		wr(sm_map + i, sm_seq[i]);
	}
	for (unsigned i = 0; i < sizeof(sm_branch_seq) / sizeof(sm_branch_seq[0]); i++) {
		wr((volatile uint32_t *)((uintptr_t)sm_map + SM_BRANCH_OFF) + i, sm_branch_seq[i]);
	}
	wr((volatile uint32_t *)((uintptr_t)sm_map + SM_START_OFF), 0x1);
	printf("wifi: the state manager started (sm %#010x, start %#010x)\n", rd(sm_map),
	    rd((volatile uint32_t *)((uintptr_t)sm_map + SM_START_OFF)));

	/* write-then-read on PARF and SM: do writes stick at all? */
	{
		volatile uint32_t *parf = parf_map;
		volatile uint32_t *sm = sm_map;

		delay_for_interval(20, NSEC_PER_MSEC);
		if (parf != NULL) {
			uint32_t orig = rd(parf + 0x1a8 / 4);        /* AXI_MSTR_WR_ADDR_HALT: safe to write */

			wr(parf + 0x1a8 / 4, 0xdeadbeef);
			uint32_t back = rd(parf + 0x1a8 / 4);
			wr(parf + 0x1a8 / 4, orig);                   /* put back */
			printf("wifi: PARF write-then-read: wrote 0xdeadbeef, read back %#010x (%s)\n", back,
			    back == 0xdeadbeef ? "WRITES STICK" : back == 0 ? "WRITES DROPPED (or unclocked)" : "writes land but not what we wrote");
		}
		if (sm != NULL) {
			/* the SM's own scratch: write something non-destructive */
			uint32_t orig2 = rd(sm + 0x30 / 4);

			wr(sm + 0x30 / 4, 0x600df00d);
			uint32_t back2 = rd(sm + 0x30 / 4);
			wr(sm + 0x30 / 4, orig2);
			printf("wifi: SM write-then-read: wrote 0x600df00d, read back %#010x (%s)\n", back2,
			    back2 == 0x600df00d ? "WRITES STICK" : "WRITES DROPPED");
		}
		/* the CRM base (0x1d01000): the PCIe Client Resource Manager v2, its
		 * own power/clock manager that does not go through gcc */
		{
			volatile uint32_t *crm = (volatile uint32_t *)ml_io_map(0x01D01000ULL, 0x2000);

			if (crm != NULL) {
				for (unsigned i = 0; i < 4; i++) {
					printf("wifi: CRM[%#04x] = %#010x\n", i * 4, rd(crm + i));
				}
			}
		}
		/* the PARF reads after the SM had its time */
		printf("wifi: after the SM: PARF[0] %#010x, PARF[0x40] %#010x, PARF[0x1b0] %#010x, PHY[0x414] %#010x\n",
		    parf_map != NULL ? rd(parf_map) : 0,
		    parf_map != NULL ? rd(parf_map + 0x40 / 4) : 0,
		    parf_map != NULL ? rd(parf_map + 0x1b0 / 4) : 0,
		    phy_map != NULL ? rd((volatile uint32_t *)((uintptr_t)phy_map + 0x414)) : 0);
	}
	/* the PARF: the controller's own setup (msm_pcie_enable_link's order) */
	if (parf_map != NULL) {
		wr(parf_map + 0x100 / 4, 0x4);                       /* DEVICE_TYPE: root complex */
		wr(parf_map + 0x20 / 4, rd(parf_map + 0x20 / 4) & ~(1u << 5));   /* l1 mode */
		wr(parf_map + 0x40 / 4, rd(parf_map + 0x40 / 4) & ~1u);          /* the PHY's clock enable */
		wr(parf_map, 0x365E);                                /* SYS_CTRL */
		wr(parf_map + 0x174 / 4, rd(parf_map + 0x174 / 4) | (1u << 4));  /* MHI clock reset */
		wr(parf_map + 0x1a8 / 4, (1u << 31) | rd(parf_map + 0x1a8 / 4)); /* write halt */
		printf("wifi: PARF set (sys %#010x, phy %#010x)\n", rd(parf_map), rd(parf_map + 0x40 / 4));
	}

	/* the PHY: the device tree's own table, then PHYSTATUS polled */
	if (phy_map != NULL) {
		for (unsigned i = 0; i < sizeof(phy_seq) / sizeof(phy_seq[0]); i++) {
			wr((volatile uint32_t *)((uintptr_t)phy_map + phy_seq[i].off), phy_seq[i].val);
			if (phy_seq[i].us) {
				delay_for_interval(phy_seq[i].us, NSEC_PER_USEC);
			}
		}
		delay_for_interval(20, NSEC_PER_MSEC);
		for (unsigned i = 0; i < 100; i++) {
			uint32_t st = rd((volatile uint32_t *)((uintptr_t)phy_map + 0x414));

			if (((st >> 6) & 1) == 0) {
				printf("wifi: the PHY is ready after %u polls (status %#010x)\n", i, st);
				break;
			}
			delay_for_interval(1, NSEC_PER_MSEC);
			if (i == 99) {
				printf("wifi: THE PHY DID NOT COME READY (status %#010x)\n", st);
			}
		}
	}

	/* the endpoint's latency, then PERST released */
	delay_for_interval(10, NSEC_PER_MSEC);
	pin_out(PIN_PERST, true);
	printf("wifi: PERST released\n");

	/* the link: LTSSM enabled */
	if (parf_map != NULL) {
		wr(parf_map + 0x1b0 / 4, rd(parf_map + 0x1b0 / 4) | (1u << 8));
		for (unsigned i = 0; i < 150; i++) {
			uint32_t ltssm = rd(parf_map + 0x1b0 / 4) & 0x3f;

			if (ltssm >= 0x10 && ltssm < 0x20) {             /* L0: the link is up */
				printf("wifi: THE LINK IS UP after %u ms (LTSSM %#x)\n", i, ltssm);
				break;
			}
			delay_for_interval(1, NSEC_PER_MSEC);
			if (i == 149) {
				printf("wifi: THE LINK DID NOT COME UP (LTSSM %#x)\n", ltssm);
			}
		}
	}
}

/* ---- the driver's start ------------------------------------------------------------------------- */
/* ---- the driver's start ------------------------------------------------------------------------- */

void
hakios_wcn_init(void)
{
	uint32_t step = 0;

	PE_parse_boot_argn("wifi_pcie", &step, sizeof(step));
	if (step == 0) {
		return;
	}
	wifi_pcie_step = step;
	printf("wifi: the WCN7850's PCIe bring-up is asked for (wifi_pcie=%u)\n", step);

	tlmm_map = (volatile uint32_t *)ml_io_map(WIFI_TLMM_PA, 0x1000 * 128);
	gcc_map = (volatile uint32_t *)ml_io_map(WIFI_GCC_PA, 0x1f4200);
	sm_map = (volatile uint32_t *)ml_io_map(0x01D07000ULL, 0x100000);
	parf_map = (volatile uint32_t *)ml_io_map(WIFI_PARF_PA, 0x3000);
	phy_map = (volatile uint32_t *)ml_io_map(0x01c06000ULL, 0x2000);
	printf("wifi: tlmm %#lx gcc %#lx sm %#lx parf %#lx phy %#lx\n", (uintptr_t)tlmm_map,
	    (uintptr_t)gcc_map, (uintptr_t)sm_map, (uintptr_t)parf_map, (uintptr_t)phy_map);
	if (tlmm_map == NULL) {
		printf("wifi: no TLMM mapping\n");
		return;
	}
	if (wifi_pcie_step >= 1) {
		stage_inspect();
		/* the controller's own blocks: PARF at 0x1c00000 (the version at
		 * +0x0 is readable when the block is clocked), the PHY at 0x1c06000
		 * (its +0x440 is the power-down register the DT names) */
		{
			volatile uint32_t *parf = (volatile uint32_t *)ml_io_map(0x01c00000ULL, 0x3000);

			if (parf != NULL) {
				uint32_t v = rd(parf);

				printf("wifi: PARF[0] %#010x (readable; +0x40 skipped -- it faulted once after a dirty run)\n", v);
			}
		}
		{
			volatile uint32_t *phy = (volatile uint32_t *)ml_io_map(0x01c06000ULL, 0x2000);

			if (phy != NULL) {
				uint32_t v = rd(phy + 0x440 / 4);

				printf("wifi: PHY power-down %#010x (readable)\n", v);
			}
		}
		/* (the gcc re-test was here: reads of gcc[0x52008]/GDSCs reliably
		 * synchronous-external-abort, fault_code 0x10, while Android reads
		 * the same registers through the same MMIO window -- our execution
		 * context genuinely does not have gcc, confirmed twice, removed so
		 * the probes after this point get their chance to run) */
		/* ---- the Gunyah probes (from research/gunyah-rm-analysis.md): P4 the
	 * virtual watchdog's state (the loader should have armed it), P1 the SCM
	 * waitqueue info (the suspected cause of the PAS_AUTH_AND_RESET wedge),
	 * P2/P3 the Resource-Manager message queues over HVC -- the RM RPC is
	 * how a guest asks the hypervisor for anything on this SoC */
	{
		/* this boot's RM capids, read by the loader out of the firmware's
		 * device tree (they change every boot -- hardcoding Android's dump
		 * got GH_ERROR_ARG_INVAL) and passed through our device tree */
		DTEntry chosen = NULL;
		void const *prop = NULL;
		unsigned int size = 0;

		rm_tx_capid = rm_rx_capid = 0;
		if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == KERN_SUCCESS &&
		    SecureDTGetProperty(chosen, "gunyah-rm-tx", &prop, &size) == KERN_SUCCESS && size == sizeof(uint64_t)) {
			rm_tx_capid = *(const uint64_t *)prop;
		}
		prop = NULL;
		if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == KERN_SUCCESS &&
		    SecureDTGetProperty(chosen, "gunyah-rm-rx", &prop, &size) == KERN_SUCCESS && size == sizeof(uint64_t)) {
			rm_rx_capid = *(const uint64_t *)prop;
		}
		printf("gh: RM capids from the loader's device tree: tx %#016llx rx %#016llx (%s)\n",
		    rm_tx_capid, rm_rx_capid,
		    rm_tx_capid != 0 ? "present" : "ABSENT -- the loader found no resource-manager node");
		/* P4: the virtual watchdog (pure SMCs, owner 6) */
		uint64_t r1 = 0, r2 = 0;
		uint64_t st = gh_call(0x86000006u, 0, 0, 0, 0, 0, &r1, &r2, NULL);

		printf("gh: vwdt status: ret %lld ctl %lld since_pet %lld\n", st, r1, r2);
		/* P1: the SCM waitqueue: GET_WQ_QUEUE_INFO, a FAST call */
		r1 = r2 = 0;
		st = gh_call(0x82002404u, 0, 0, 0, 0, 0, &r1, &r2, NULL);
		printf("gh: waitq GET_WQ_QUEUE_INFO: ret %lld call_ctx_cnt %lld irq %lld (994 = the wake IRQ we must serve)\n",
		    st, r1, r2);
		/* P2/P3 -- the RM message queues -- are DANGEROUS with real capids:
		 * a recv on a valid queue parked the vCPU at EL2 and froze the SoC
		 * (recovered by the watchdog). Gate the actual exchange behind
		 * gh_rpc=1; without it we only report the capids. */
		int gh_rpc = 0;

		(void) PE_parse_boot_argn("gh_rpc", &gh_rpc, sizeof(gh_rpc));
		static uint64_t rm_buf[30] __attribute__((aligned(8)));

		if (rm_rx_capid != 0 && gh_rpc) {
			uint64_t r1 = 0, r2 = 0;
			uint64_t st = gh_call(0x601Cu, rm_rx_capid, (uint64_t)(uintptr_t)rm_buf, 240, 0, 2, &r1, &r2, NULL);

			printf("gh: RM rx msgq recv: ret %lld (0 = a message waited, 60 = empty, 61 = full), len %lld not_empty %lld\n",
			    st, r1, r2);
			if (st == 0) {
				printf("gh: RM rx drained: %#016llx %#016llx %#016llx\n", rm_buf[0], rm_buf[1], rm_buf[2]);
			}
		}
		/* P3: the full round trip: VM_GET_VMID (msg_id 0x56000024, empty
		 * payload) on the tx queue. The 8-byte RPC header: api 0x21,
		 * type REQUEST (1), seq 1, then the message id. */
		if (rm_tx_capid != 0 && gh_rpc) {
			static const uint64_t vmid_req = 0x5600002400010121ULL;
			uint64_t r1 = 0, r2 = 0;
			uint64_t st = gh_call(0x601Bu, rm_tx_capid, 8, (uint64_t)(uintptr_t)&vmid_req, 1, 2, &r1, &r2, NULL);

			printf("gh: RM tx send VM_GET_VMID: ret %lld not_full %lld\n", st, r1);
			if (st == 0) {
				for (unsigned i = 0; i < 2000; i++) {    /* 2 s for the reply */
					r1 = r2 = 0;
					st = gh_call(0x601Cu, rm_rx_capid, (uint64_t)(uintptr_t)rm_buf, 240, 0, 2, &r1, &r2, NULL);
					if (st == 0) {
						printf("gh: RM reply after %u us, len %lld: hdr %#016llx err+vmid %#016llx (%s)\n",
						    i, r1, rm_buf[0], rm_buf[1],
						    ((rm_buf[1] >> 32) & 0xffff) == 3 ? "vmid 3 -- we are the primary VM, the whole RM stack works" :
						    "unexpected vmid");
						break;
					}
					if (i == 1999) {
						printf("gh: RM VM_GET_VMID never replied (last ret %lld)\n", st);
					}
					delay_for_interval(1, NSEC_PER_USEC);
				}
			}
		}
	}
	/* ---- the GPU probe: the annibale (Redmi K90, same SM8750) port drives
	 * the GPU entirely through these four blocks -- gpucc 0x3d90000 (its
	 * OWN clock controller with the TAYCAN_ELU PLLs and the CX GDSC, not
	 * the central gcc that aborts for us), the GMU window 0x3d37000, the
	 * GPU block 0x3d00000 and the GPU's own SMMU 0x3da0000. None of this
	 * window was ever probed on this phone. Read-only; an abort would say
	 * the OEM filter reaches here too, a value says the GPU path is open. */
	{
		static const struct {
			const char *name;
			uint64_t pa;
			uint32_t offs[3];
			const char *labels[3];
		} gpu_blocks[] = {
			{ "gpucc", 0x03d90000ULL, { 0x9080, 0x931c, 0x93f0 }, { "CX_GDSCR(bit31=up)", "GMU_RCGR src", "HUB_RCGR src" } },
			{ "gmu", 0x03d37000ULL, { 0x0, 0x1f8, 0x2004 }, { "id", "version", "ao" } },
			{ "gpu", 0x03d00000ULL, { 0x0, 0x11c, 0x2054 }, { "id", "cp id", "sqe cntl" } },
			{ "gsmmu", 0x03da0000ULL, { 0x20, 0x24, 0x28 }, { "IDR0", "IDR1", "IDR2" } },
		};

		for (unsigned b = 0; b < sizeof(gpu_blocks) / sizeof(gpu_blocks[0]); b++) {
			volatile uint32_t *m = (volatile uint32_t *)ml_io_map(gpu_blocks[b].pa, 0x10000);

			if (m == NULL) {
				continue;
			}
			for (unsigned i = 0; i < 3; i++) {
				printf("gpu: %s[%#06x] (%s) %#010x\n", gpu_blocks[b].name, gpu_blocks[b].offs[i], gpu_blocks[b].labels[i],
				    rd(m + gpu_blocks[b].offs[i] / 4));
			}
		}
		/* the legacy SCM IO service (svc 5): TrustZone does the MMIO for
		 * the caller -- if this firmware still serves it, we can reach the
		 * filtered central gcc through EL3 as well */
		{
			uint64_t r1 = 0;
			uint64_t st = gh_call(0xC2000501u, 0x01052008ULL, 0, 0, 0, 0, &r1, NULL, NULL);

			printf("gpu: SCM_IO read gcc[0x52008]: ret %lld, value %#010llx (a real value = the EL3 path works)\n", st, r1);
			r1 = 0;
			st = gh_call(0xC2000501u, 0x03d99080ULL, 0, 0, 0, 0, &r1, NULL, NULL);
			printf("gpu: SCM_IO read gpucc CX_GDSCR: ret %lld, value %#010llx\n", st, r1);
		}
	}
	/* the SCMI shared memory (0x17b4e000/0x200 tx, 0x17b4e200/0x200 rx):
		 * the ARM System Control and Management Interface -- the Linux gcc
		 * driver talks through here (protocol 13 = clocks), and it bypasses
		 * the gcc register window that Gunyah filters for this VM */
		{
			volatile uint32_t *tx = (volatile uint32_t *)ml_io_map(0x17b4e000ULL, 0x200);
			volatile uint32_t *rx = (volatile uint32_t *)ml_io_map(0x17b4e200ULL, 0x200);

			if (tx != NULL && rx != NULL) {
				printf("wifi: SCMI tx[0..3] %#010x %#010x %#010x %#010x\n", rd(tx), rd(tx + 1), rd(tx + 2), rd(tx + 3));
				printf("wifi: SCMI rx[0..3] %#010x %#010x %#010x %#010x\n", rd(rx), rd(rx + 1), rd(rx + 2), rd(rx + 3));
				/* the CPUCP (phandle 0x4a): the SCMI doorbell lives here
				 * (rx at 0x16430000, tx at 0x17830000) */
				{
					volatile uint32_t *cpucp = (volatile uint32_t *)ml_io_map(0x01783000ULL, 0x300);

					if (cpucp != NULL) {
						for (unsigned i = 0; i < 4; i++) {
							printf("wifi: CPUCP[%#04x] %#010x\n", i * 4, rd(cpucp + i));
						}
						/* the definitive diagnostic: prime the payload with a
						 * fingerprint, use interrupt-mode flags like Linux's
						 * default exchange, ring, then watch BOTH the tx shmem
						 * free bit and the CPUCP rx-side status_reg (the v2
						 * completion path: firmware sets a bit at rx+0x4400,
						 * the AP reads rx+0x100, clears at rx+0x4800, and only
						 * then looks at the response in the tx shmem) */
						{
							volatile uint32_t *shmem = tx;       /* the tx channel */
							volatile uint32_t *rxwin = (volatile uint32_t *)ml_io_map(0x16430000ULL, 0x5000);
							uint32_t answered = 0;

							printf("wifi: SCMI before: tx[4..9] %#010x %#010x %#010x %#010x %#010x %#010x\n",
							    rd(shmem + 4), rd(shmem + 5), rd(shmem + 6), rd(shmem + 7), rd(shmem + 8), rd(shmem + 9));
							if (rxwin != NULL) {
								printf("wifi: SCMI CPUCP rx window: status[0x4400] %#010x %#010x, data[0x100] %#010x %#010x\n",
								    rd(rxwin + 0x4400 / 4), rd(rxwin + 0x4404 / 4), rd(rxwin + 0x100 / 4), rd(rxwin + 0x104 / 4));
							}
							/* NOTE: never read the doorbell slots 0x100..0x13f here —
							 * reading a doorbell command register can fire it, and
							 * ringing channels 1..7 wedged the whole SoC (the
							 * build that dumped them hung with no log; the loader's
							 * watchdog had to reset the phone) */
							wr(shmem + 1, 0);                     /* mark busy */
							wr(shmem + 4, 1);                     /* flags: INTR_ENABLED, Linux's default */
							wr(shmem + 5, 4);                     /* length: just the header */
							wr(shmem + 6, (0x22u << 18) | (0u << 10) | (0u << 8) | 0u); /* base protocol, VERSION */
							wr(shmem + 7, 0x12345678);            /* the fingerprint: if this changes, the firmware wrote here */
							__asm__ volatile ("dsb sy" ::: "memory");
							/* Linux's mailbox startup enables the channel's RX
							 * interrupt bit at rx+0x4C00 before anything is
							 * sent; we never did. If the CPUCP only serves
							 * started channels, this is what was missing. */
							if (rxwin != NULL) {
								uint64_t en = ((uint64_t)rd(rxwin + 0x4c00 / 4)) | ((uint64_t)rd(rxwin + 0x4c04 / 4) << 32);

								en |= 1;                        /* channel 0 */
								wr(rxwin + 0x4c00 / 4, (uint32_t)en);
								wr(rxwin + 0x4c04 / 4, (uint32_t)(en >> 32));
								printf("wifi: SCMI CPUCP channel-0 enable written (%#010x %#010x)\n",
								    rd(rxwin + 0x4c00 / 4), rd(rxwin + 0x4c04 / 4));
							}
							wr((volatile uint32_t *)((uintptr_t)cpucp + 0x104), 1); /* doorbell: transfer_id 1; no readback — a read may re-fire it */
							for (unsigned i = 0; i < 3000; i++) {  /* 3 s for the answer */
								uint32_t status = rd(shmem + 1);

								if (status & 1) {                 /* free again = the response is in */
									printf("wifi: SCMI VERSION answered after %u us: status %#010x, len %u, hdr %#010x, payload %#010x %#010x\n",
									    i, status, rd(shmem + 5), rd(shmem + 6), rd(shmem + 7), rd(shmem + 8));
									answered = 1;
									break;
								}
								if ((i % 500) == 499 && rxwin != NULL) {
									uint32_t s = rd(rxwin + 0x4400 / 4);

									printf("wifi: SCMI at %u us: tx_status %#010x, payload %#010x, rx status_reg %#010x, rx data %#010x\n",
									    i, status, rd(shmem + 7), s, rd(rxwin + 0x100 / 4));
									if (s != 0) {
										/* the AP ack sequence: clear the bits, then the response may appear */
										wr(rxwin + 0x4800 / 4, s);
										wr(rxwin + 0x4804 / 4, rd(rxwin + 0x4404 / 4));
										printf("wifi: SCMI CPUCP completion bit(s) %#010x cleared; tx_status now %#010x, payload %#010x\n",
										    s, rd(shmem + 1), rd(shmem + 7));
									}
								}
								delay_for_interval(1, NSEC_PER_USEC);
							}
							if (!answered) {
								printf("wifi: SCMI VERSION never freed the channel; final tx[4..9] %#010x %#010x %#010x %#010x %#010x %#010x, rx status %#010x\n",
								    rd(shmem + 4), rd(shmem + 5), rd(shmem + 6), rd(shmem + 7), rd(shmem + 8), rd(shmem + 9),
								    rxwin != NULL ? rd(rxwin + 0x4400 / 4) : 0);
							}
						}
					}
				}
			}
		}
	}
	/* the gcc probe: the SAME reads external-aborted (fault_code 0x10) in
	 * every fastboot-booted session; this kernel now boots through the
	 * normal chain (flashed boot_a + set_active) -- if the restriction was
	 * the boot mode, these now return real values (the K90 port reads this
	 * very window daily). abort returns to the loader cleanly either way */
	{
		volatile uint32_t *g = (volatile uint32_t *)ml_io_map(0x01000000ULL, 0x1f4200);

		if (g != NULL) {
			printf("wifi: gcc[0x52000] %#010x, gcc[0x52008] %#010x, gcc[0x6b004] %#010x, gcc[0x6c000] %#010x\n",
			    rd(g + 0x52000 / 4), rd(g + 0x52008 / 4), rd(g + 0x6b004 / 4), rd(g + 0x6c000 / 4));
		}
	}
	if (wifi_pcie_step >= 2) {
		stage_cesta();
	}
	/* stages 3 and on: the clock branches, the PHY, PARF, the link — written
	 * as each is proven */
}
