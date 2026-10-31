/* Shared register definitions, state and declarations for the SM8750 GPU probes. */
#ifndef _DEV_ADRENO8_PRIVATE_H
#define _DEV_ADRENO8_PRIVATE_H

#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include "adreno8_wire_private.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern void loader_return(void);
extern void loader_log_putc(char c);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

struct adreno8_block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

struct adreno8_cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};

struct adreno8_cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct adreno8_cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};

struct adreno8_cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* CMD DB BCM wire layout, little-endian (Linux include/soc/qcom/bcm.h). */
struct adreno8_bcm_db {
	uint32_t unit;
	uint16_t width;
	uint8_t vcd;
	uint8_t reserved;
};

struct adreno8_gpu_level {
	uint32_t hz, level, bus;        /* bus: the stock table's qcom,bus-freq, a row of adreno8_bw_kb */
};

struct adreno8_bcm {
	const char *name;
	uint32_t buswidth;      /* of the connection: catalog's bcm buswidth */
	bool fixed;             /* votes "perfmode", not a computed amount */
	uint32_t perfmode, perfmode_bw;
};

struct adreno8_gpu_block {
	const char *name;
	uint32_t address, size;
	vm_offset_t memory;                     /* kernel memory; pages need not be neighbors */
};

struct adreno8_space_table {
	vm_offset_t table;
	uint64_t phys;
	uint32_t index;                 /* the range it translates, by level */
	unsigned level;                 /* 0 root, 1, 2 */
};

struct adreno8_gpu_space {
	const char *name;
	uint32_t context;
	struct adreno8_space_table tables[1024];
	unsigned table_count;
	uint16_t leaf_slot[2048];                 /* each 2 MB's level-2 table, as its index in tables[] + 1; 0: not looked up yet */
	unsigned secure_tables;           /* pages already assigned to {HLOS read/write, CP_PIXEL read} */
	bool is_on;
};

#define GCC_DDRSS_GPU_AXI       0x71150         /* branch2_aon: memory clock (gcc-sm8750.c); its halt report is unreliable */
#define GCC_GPU_GEMNOC_GFX      0x71010         /* gcc-sun.c: voted branch2_aon; GMU memnoc_clk */
#define CBCR_ENABLE             (1u << 0)       /* a clock's control register: on */
#define CBCR_HWCG               (1u << 1)
#define CBCR_NOC_STATUS         (7u << 28)
#define CBCR_NOC_ON             (2u << 28)
#define CBCR_OFF                (1u << 31)      /* ... and whether it is in fact off */
#define CMD_DB_PHYS             0x81c60000UL
#define CMD_DB_SIZE             0x20000UL
#define RPMH_LEVELS             64              /* levels of one rail's list (redwood: 16; gfx.lvl is longer here) */
#define QFPROM_GPU_SPEED_BIN    0x138
#define BW_COUNT                12
#define GPUCC_CX_GDSCR          0x9080          /* bit 0 collapse asked, bit 1 under hardware control, bit 31 up */
#define GPUCC_CX_HW_CTRL        0x9094          /* what the hardware itself votes (the power domain driver ORs the two) */
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_HW_CONTROL         (1u << 1)
#define GDSC_POWER_ON           (1u << 31)
#define GPUCC_AHB_CBCR          0x90bc          /* the register bus: reports its halt untrustworthily */
#define GPUCC_CX_CBCR           0x90e4          /* cxo: the crystal's clock into the GPU block */
#define GPUCC_CX_GMU_CBCR       0x90d4          /* the GMU's */
#define GPUCC_GX_GMU_CBCR       0x9060
#define GPUCC_GX_ACCU_CBCR      0x9070
#define GPUCC_CX_ACCU_CBCR      0x910c
#define GPUCC_DEMET_CBCR        0x9010
#define GPUCC_DPM_CBCR          0x9110
#define GPUCC_FREQ_MEASURE      0x900c
#define GPUCC_SMMU_VOTE_CBCR    0x7000          /* hlos1_vote_gpu_smmu */
#define GPUCC_MEMNOC_GFX_CBCR   0x90f4
#define GPUCC_HUB_AON_CBCR      0x93e8
#define GPUCC_HUB_CX_INT_CBCR   0x90e8
#define GPUCC_GMU_BCR           0x9314          /* GMU block reset; bit 0 asserts reset */
#define GPUCC_GMU_RCGR          0x9318          /* its clock source: parents 0 the crystal, 2 PLL0_OUT_EVEN */
#define GPUCC_HUB_RCGR          0x93ec
#define GPUCC_HUB_DIV           0x942c
#define RCG_SOURCE_MASK         0x371f          /* [10:8] the source, [4:0] the divider - 1 (redwood's mask, same format) */
#define RCG_UPDATE              (1u << 0)       /* clk-rcg2 CMD_UPDATE; bit 31 reports ROOT_OFF */
#define CX_MISC_TCM_RET         0x00e4          /* 1: the GMU's memories were kept: a warm start */
#define CX_MISC_SW_FUSE         0x1000          /* feature bits (Linux: not a speed limit) */
#define RSCC(n)                 (4u * (n))      /* the vendor names its registers by dword too */
#define RSCC_DRV_CTRL           RSCC(0xc)
#define RSCC_RSC_STATUS         RSCC(0x4)       /* bit 16: the RSC is asleep */
#define RSCC_SEQ_BUSY           RSCC(0x101)
#define RSCC_ASLEEP             (1u << 16)      /* set means asleep, as in Linux's RSC_STATUS_PM_STATUS */
#define GBIF_HALT               0x114           /* window bytes: a6xx gbif's halt/ack pair (word 0x45/0x46) */
#define GBIF_HALT_ACK           0x118
#define GMU(n)                  (4u * (n) - 0x37000u)   /* xml dword index -> byte in this window */
#define GPU_GBIF_CX_CONFIG      0xf000          /* the four QSB sides are the words after it */
#define GPU_GBIF_HALT           0xf114          /* GEN8_GBIF_HALT, dword 0x3c45 */
#define GPU_GBIF_QSB_SIDE0      0xf00c          /* gen8_0_0_gbif_cx_regs: all four use 0x00071e20 */
#define GPU_GBIF_QSB_VALUE      0x00071e20
#define GPU_GBIF_CX_VALUE       0x20023000
#define GMU_ITCM                GMU(0x1b400)    /* the processor's two memories, mapped into its registers */
#define GMU_DTCM                GMU(0x1c400)
#define GMU_DTCM_VERSIONS       GMU(0x1c400 + 0xff8)    /* the firmware leaves its three versions at its data memory's end */
#define GMU_DTCM_ADDRESS        0x10004000
#define GMU_ICACHE_CONFIG       GMU(0x1f400)
#define GMU_DCACHE_CONFIG       GMU(0x1f401)
#define GMU_SYS_BUS_CONFIG      GMU(0x1f40f)
#define GMU_MRC_GBIF_QOS        GMU(0x1f50b)
#define GMU_PWR_CLK_STATUS      GMU(0x1f7e8)    /* bit 0: the 3D core's supply off; bit 1: its clock off */
#define GMU_PWR_GX_UP(mask)     (((mask) & 3) == 0)
#define GMU_RPMH_POWER_STATE    GMU(0x1f7e9)
#define GMU_CX_FAL_INTF         GMU(0x1f7ec)
#define GMU_CX_FALNEXT_INTF     GMU(0x1f7ed)
#define GMU_CM3_SYSRESET        GMU(0x1f800)
#define GMU_CM3_BOOT_CONFIG     GMU(0x1f801)
#define GMU_FW_INIT_RESULT      GMU(0x1f81c)    /* its firmware's own verdict, once it runs */
#define GMU_FW_STARTED          0x100           /* the word newer firmware must show... */
#define GMU_FW_STARTED_MASK     0x1ff
#define GMU_FW_FAILED           0xe00
#define GMU_FW_ALREADY          0xbabeface      /* ... and older firmware's "I am already running" */
#define GMU_CM3_CFG             GMU(0x1f82d)
#define GMU_ALWAYS_ON_HI        GMU(0x1f841)
#define GMU_ALWAYS_ON_LO        GMU(0x1f840)    /* the GMU's counter: crystal ticks */
#define GMU_HFI_CTRL_STATUS     GMU(0x1f980)    /* bit 0: the queues are up */
#define GMU_HFI_QTBL_INFO       GMU(0x1f984)
#define GMU_HFI_QTBL_ADDR       GMU(0x1f985)
#define GMU_HFI_CTRL_INIT       GMU(0x1f986)
#define GMU_GMU2HOST_SET        GMU(0x1f990)
#define GMU_GMU2HOST_CLR        GMU(0x1f991)    /* writing bit 0 clears "a message is here" */
#define GMU_GMU2HOST_INFO       GMU(0x1f992)    /* bit 0: a message; bit 23: the firmware faults */
#define GMU_GMU2HOST_MASK       GMU(0x1f993)    /* a set bit keeps the message quiet */
#define GMU_HOST2GMU_SET        GMU(0x1f994)    /* bit 0: go and look at the queue */
#define GMU_GENERAL_8           GMU(0x1f9c8)    /* where the GMU's log is, and its pages minus one */
#define GMU_GENERAL_9           GMU(0x1f9c9)    /* ... and how far it has written */
#define GMU_GENERAL_10          GMU(0x1f9ca)    /* which chip the firmware is told it runs on */
#define GMU_GENERAL_11          GMU(0x1f9cb)    /* GMU VA of the virtual register bank */
#define GMU_AO_INTERRUPT_CLR    GMU(0x23b04)
#define GMU_AO_INTERRUPT_STATUS GMU(0x23b05)    /* bit 0 watchdog bit, bit 3 fence error, bit 5 bus error */
#define GMU_AO_INTERRUPT_MASK   GMU(0x23b06)
#define GMU_RSCC_CONTROL_REQ    GMU(0x23b07)    /* bit 1: ours, for the RSC to wake */
#define GMU_RSCC_CONTROL_ACK    GMU(0x23b08)
#define GMU_AHB_FENCE_CTRL      GMU(0x23b10)    /* 0: a forbidden access is answered, 1: it fences the bus */
#define GMU_AHB_FENCE_RANGE_0   GMU(0x23b11)    /* the GMU is forbidden the 3D core's registers until it has them right */
#define GMU_AHB_FENCE_STATUS    GMU(0x23b13)
#define GMU_AHB_FENCE_CLR       GMU(0x23b14)
#define GMU_RBBM_INT_UNMASKED   GMU(0x23b15)
#define GMU_BLOCKS              12
#define GMU_BLOCK_CAP           0x01000000      /* a demand of up to 16M (the gen8 cache memory is 16M - 16K) */
/* The SMMU uses 4 KiB granules independently of the XNU allocation page size.
 * MP_ASSIGN shares the granule and physical-range rules, so they live here. */
#define SMMU_GRANULE            4096u
#define SMMU_GRANULE_MASK       (SMMU_GRANULE - 1)
#define SMMU_PHYS_LIMIT         (1ULL << 36) /* TCR2.PASIZE = 1; IDR2 on SM8750 reports 36 bits. */
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_IDR2               0x028
#define SMMU_IDR7               0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))               /* in the second page; [17:16] kind of context */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))       /* GR1 register; bit 0 selects 64-bit translation */
#define SMMU_CB_SCTLR           0x00            /* a context's own page: bit 0 = translation on */
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR_LO          0x60
#define SMMU_CB_FAR_HI          0x64
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBASID         0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4           /* bit 0: a sync is still on its way */
#define SCTLR_ON                (1u | 2u | 4u | 32u) /* M, TRE, AFE and CFRE; faults are polled; ASIDPNE hint disabled */
#define SCTLR_CFCFG             (1u << 7)       /* SMMUv2 context fault configuration: stall instead of terminate */
#define SCTLR_M                 1u              /* CB SCTLR bit 0: stage-1 translation on; the secure preflight reads only this bit of context 3 */
#define TCR_39_BITS             (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)     /* 39 bits, three levels (dt: qcom,use-3-lvl-tables) */
#define TCR2_39_BITS            (1u | 1u << 4 | 7u << 15) /* 36-bit PA, 16-bit ASID, upstream separation */
#define MAIR0_NORMAL            0xf404ff44
#define FSR_ALL                 0xc00001fe      /* every fault bit cleared by writing ones */
#define SMR_VALID               (1u << 31)
#define SMR_MASK                0u              /* annibale GMU iommus: SID 5, mask 0 */
#define STAGE_1                 (1u << 16 | 0xfu << 12 | 3u << 8) /* S1 translation, S2 bypass; use stage-1 memory attributes */
#define PTE_TABLE               3u              /* a table, the next level down */
/* AArch64 SMMU treats EL0-writable pages as PXN; GMU code requires privileged-only RW. */
#define PTE_PAGE                (3u | 3u << 8 | 1u << 10 | 1u << 11) /* privileged RW, inner-shareable, AF, nG; MAIR index 0 */
#define PTE_ADDRESS             0x0000fffffffff000ULL
#define GMU_STREAM              5
#define GMU_MATCH               1
#define GMU_CONTEXT             2              /* vendor reserves contexts 0 and 1 for GPU SID 0/1 */
#define SECURE_CONTEXT          3              /* candidate for the gfx3d_secure SID 2 (vendor sun-gpu.dtsi); occupancy is re-checked by adreno8_secure_preflight before every secure handover; secure-world reservations of a context bank stay invisible to HLOS */
#define SECURE_STREAM           2              /* gfx3d_secure stream id (vendor sun-gpu.dtsi, same SMMU as GMU SID 5) */
#define SECURE_MATCH            2              /* candidate match slot: gfx3d_secure is the third stream-match entry in the vendor dtsi; a device scan confirms the slot the boot loader actually assigned */
#define BCM_TCS_CMD(commit, valid, vote_x, vote_y) \
	(((commit) ? 1u << 30 : 0u) | ((valid) ? 1u << 29 : 0u) | (((vote_x) & 0x3fffu) << 14) | ((vote_y) & 0x3fffu))
#define BCM_COUNT               3                   /* the rows in adreno8_bcms: SH0, MC0, ACV */

extern uint32_t adreno8_skip_steps;
extern uint32_t adreno8_pause_ms;
extern uint32_t adreno8_speed_asked;
extern uint32_t adreno8_inflight;
extern uint32_t adreno8_gmu_from_pll;
extern bool adreno8_gpu_stall;
extern uint32_t adreno8_step;
extern uint32_t adreno8_newest_step;
extern char adreno8_last_write[96];
extern bool adreno8_quiet;
extern uint32_t adreno8_speed_bin;
extern const struct adreno8_gpu_level *adreno8_speeds;
extern unsigned adreno8_speeds_count;
extern uint32_t adreno8_gx_hz[17];
extern uint32_t adreno8_gx_level[17];
extern unsigned adreno8_gx_count;
extern const uint32_t adreno8_gmu_hz[3];
extern const uint32_t adreno8_gmu_level[3];
extern const uint32_t adreno8_bw_kb[BW_COUNT];
extern const struct adreno8_bcm adreno8_bcms[];
extern uint32_t adreno8_gx_votes[17];
extern uint32_t adreno8_gx_dep_votes[17];
extern uint32_t adreno8_cx_votes[3];
extern struct adreno8_block adreno8_gpucc;
extern bool adreno8_cx_changed;
extern uint32_t adreno8_cx_saved_gdscr;
extern struct adreno8_block adreno8_cx_misc;
extern struct adreno8_block adreno8_rscc;
extern bool adreno8_rsc_asleep_at_first;
extern struct adreno8_block adreno8_gmu;
extern struct adreno8_block adreno8_gpu;
extern struct adreno8_gpu_block adreno8_gmu_demands[GMU_BLOCKS];
extern unsigned adreno8_gmu_demand_count;
extern struct adreno8_gpu_block adreno8_gmu_log;
extern struct adreno8_gpu_block adreno8_gmu_hfi;
extern bool adreno8_gmu_runs;
extern bool adreno8_gmu_talks;
extern bool adreno8_direct_ring;
/* Cold diagnostic: START can be attempted once; HFI failure permanently clears ready. */
bool adreno8_ordinary_start(void);
bool adreno8_ordinary_ready(void);
extern bool adreno8_fw_loaded;
extern bool adreno8_rsc_woken_by_us;
extern vm_offset_t adreno8_log_view[4];
extern vm_offset_t adreno8_hfi_view_table, adreno8_hfi_view_cmd, adreno8_hfi_view_resp;
extern vm_offset_t adreno8_hfi_view_debug;
extern struct adreno8_gpu_space adreno8_gmu_space;
extern struct adreno8_gpu_space adreno8_secure_space;
extern bool adreno8_gpu8_keep;
extern bool adreno8_reset_required;
extern bool adreno8_gpu8_return;
extern uint32_t adreno8_gpu8_secure_mode; /* boot-args adreno8_secure_preflight=N; unknown values parse to 0 */
#define SECURE_MODE_OFF          0u
#define SECURE_MODE_PREFLIGHT    1u            /* answer kind 5 with the read-only preflight only, no MP_ASSIGN and no reply */
#define SECURE_MODE_HANDOVER     2u            /* isolated experiment: one handover of the kernel's own pages, terminal afterwards, no reply and no CB3 write */
#define SECURE_MODE_TRANSLATION_A 3u           /* 3a: context 3 pointed at the handed-over tables and enabled, stream 2 route kept at fault, register read-backs and a clear FSR as the only proof this SMMU offers */
#define SECURE_MODE_TRANSLATION_B 4u           /* 3b: enable the secure route, then reply to one kind-5 allocation */
bool adreno8_memory_retained(void);

int
adreno8_say(const char *format, ...);
void
adreno8_sleep_us(uint32_t us);
void
adreno8_sleep_ms(uint32_t ms);
uint32_t
adreno8_rd(vm_offset_t address);
void
adreno8_wr(vm_offset_t address, uint32_t value);
void
adreno8_need(struct adreno8_block *b);
void
adreno8_hold(bool single_access);
void
adreno8_announce(const char *format, ...);
uint32_t
adreno8_get(struct adreno8_block *b, const char *name, uint32_t offset);
void
adreno8_put(struct adreno8_block *b, const char *name, uint32_t offset, uint32_t value);
uint32_t
adreno8_set(struct adreno8_block *b, const char *name, uint32_t offset, uint32_t bits);
bool
adreno8_wait_for(struct adreno8_block *b, uint32_t offset, uint32_t mask, uint32_t value, uint32_t ms);
uint32_t
adreno8_rpmh_lookup(const char *name, uint16_t *levels, unsigned *level_count, const uint8_t **data, unsigned *data_length);
void
adreno8_show_rpmh_resource(const char *name);
void
adreno8_show_rpmh_bcm(const char *name);
bool
adreno8_arc_vote(const char *pri_n, const char *sec_n, uint32_t level, uint32_t *vote);
bool
adreno8_dep_vote(uint32_t level, uint32_t *vote);
bool
adreno8_step_outside(void);
bool
adreno8_gpucc_answers(void);
bool
adreno8_cx_is_on(void);
bool
adreno8_step_gpucc(void);
bool
adreno8_clock_on(struct adreno8_block *b, const char *name, uint32_t cbcr, bool must);
bool
adreno8_clock_source(const char *name, uint32_t rcgr, uint32_t source);
bool
adreno8_cx_ready(void);
bool
adreno8_step_cx_power(void);
bool
adreno8_step_cx_awake(void);
bool
adreno8_step_clocks(void);
bool
adreno8_scm_gpu_init(void);
bool
adreno8_scm_gpu_aperture(void);
int32_t
adreno8_scm_call(uint64_t function, uint64_t arginfo, uint64_t argument0,
    uint64_t argument1, uint64_t argument2, uint64_t argument3, uint64_t *answer);
bool
adreno8_block_new(struct adreno8_gpu_block *b, const char *name, uint32_t address, uint32_t size);
void
adreno8_settle(void);
void
adreno8_to_memory(vm_offset_t start, vm_size_t size);
bool
adreno8_covered(uint32_t address, uint32_t size);
vm_offset_t
adreno8_seen_at(uint32_t address);
void
adreno8_gmu_post_mortem(void);
bool
adreno8_step_first_words(void);
bool
adreno8_step_gmu_memory(void);
bool
adreno8_step_gmu_space(void);
bool
adreno8_step_firmware(void);
bool
adreno8_step_gmu_start(void);
bool
adreno8_step_smmu_read(void);
void
adreno8_smmu_cb_scan_ask(void);
void
adreno8_smmu_cb_scan_after_step9(void);
vm_offset_t
adreno8_space_new_table(struct adreno8_gpu_space *space, unsigned level, uint32_t index);
bool
adreno8_space_map(struct adreno8_gpu_space *space, uint32_t address, uint64_t phys);
bool
adreno8_space_map_block(struct adreno8_gpu_space *space, const struct adreno8_gpu_block *b);
bool
adreno8_space_on(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match);
bool
adreno8_space_route(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match);
bool
adreno8_space_route_check(struct adreno8_gpu_space *space, uint32_t stream, uint32_t match);
void
adreno8_space_off(struct adreno8_gpu_space *space, uint32_t match);
bool
adreno8_space_faulted(struct adreno8_gpu_space *space);
bool
adreno8_translation_walk(struct adreno8_gpu_space *space, uint32_t iova, vm_offset_t memory,
    uint32_t size, uint32_t prot);
bool
adreno8_secure_translation_3a(struct adreno8_gpu_space *space);
bool
adreno8_secure_translation_3b(struct adreno8_gpu_space *space);
bool
adreno8_step_queues(void);
bool
adreno8_step_tables(void);
bool
adreno8_step_gx_power(void);
void
adreno8_speed_tick(void);
extern uint64_t adreno8_retired_total;
void
adreno8_speed_state(uint32_t *mhz, uint32_t *changes);
bool
adreno8_step_last_look(void);
bool
adreno8_context_test(void);
bool
adreno8_step_service(void);

#endif /* _DEV_ADRENO8_PRIVATE_H */
