/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * SM8750 gen8_0_0 cold-boot command probe, based on Qualcomm's gen8_start,
 * gen8_patch_pwrup_reglist and gen8_hwsched_cp_init. Source revision and
 * register-table copyright notices are in adreno8_gpu_private.h.
 * Every allocation and the GPU OOB request remain owned until cold reset.
 */
#include "adreno8_private.h"

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
#include "adreno8_memory_private.h"
#include "adreno8_hwsched_private.h"
#include "adreno8_gpu_private.h"
#include "adreno8_context_private.h"

extern void loader_log_line(const char *line);
extern struct adreno8_block adreno8_smmu;

#define printf adreno8_say
#define A8_COUNT(a) (sizeof(a) / sizeof((a)[0]))

enum gpu_state { GPU_COLD, GPU_PREPARED, GPU_CP_READY, GPU_UNSECURE, GPU_TESTED, GPU_FAILED };
static enum gpu_state gpu_state;
static struct adreno8_memory *sqe, *aqe, *powerup, *scratch, *ring;
static bool oob_requested, oob_acked;

static bool
gpu_fail(const char *what)
{
	gpu_state = GPU_FAILED;
	printf("adreno8: GPU %s failed; OOB requested=%u acknowledged=%u; resources retained until cold reset\n",
	    what, oob_requested, oob_acked);
	return false;
}

/* gen8_start and kgsl_regmap_read/write at d50b305f7da9 use ordered readl/writel.
 * Keep their before-write/after-read ordering for GPU and GMU/AO accesses;
 * adreno8_settle supplies the stronger dsb sy in both diagnostic and direct paths. */
static uint32_t
gpu_read(uint32_t reg)
{
	uint32_t value;
	if (gpu_state == GPU_FAILED)
		return 0;
	if (!adreno8_gpu.base || reg >= adreno8_gpu.size / sizeof(uint32_t)) {
		gpu_fail("register read outside mapped aperture");
		return 0;
	}
	if (adreno8_pause_ms && adreno8_step == adreno8_newest_step)
		value = adreno8_get(&adreno8_gpu, "command register", 4u * reg);
	else
		value = adreno8_rd(adreno8_gpu.base + 4u * reg);
	adreno8_settle();
	return value;
}

static void
gpu_write(uint32_t reg, uint32_t value)
{
	if (gpu_state == GPU_FAILED)
		return;
	if (!adreno8_gpu.base || reg >= adreno8_gpu.size / sizeof(uint32_t)) {
		gpu_fail("register write outside mapped aperture");
		return;
	}
	adreno8_settle();
	if (adreno8_pause_ms && adreno8_step == adreno8_newest_step)
		adreno8_put(&adreno8_gpu, "command register", 4u * reg, value);
	else
		adreno8_wr(adreno8_gpu.base + 4u * reg, value);
}

static uint32_t
gpu_gmu_read(uint32_t offset)
{
	uint32_t value = adreno8_rd(adreno8_gmu.base + offset);
	adreno8_settle();
	return value;
}

static void
gpu_gmu_write(uint32_t offset, uint32_t value)
{
	adreno8_settle();
	adreno8_wr(adreno8_gmu.base + offset, value);
}

static bool
gpu_aperture(uint32_t pipe)
{
	gpu_write(GEN8_CP_APERTURE_CNTL_HOST, pipe << 12);
	adreno8_settle();
	if (gpu_read(GEN8_CP_APERTURE_CNTL_HOST) != pipe << 12)
		return gpu_fail("aperture readback");
	return gpu_state != GPU_FAILED;
}

static bool
gpu_pipe_write(uint32_t pipe, uint32_t reg, uint32_t value)
{
	if (!gpu_aperture(pipe))
		return false;
	gpu_write(reg, value);
	return gpu_state != GPU_FAILED;
}

static bool
gpu_accessible(void)
{
	if (gpu_state == GPU_FAILED || !oob_acked ||
	    !(adreno8_direct_ring ? adreno8_ordinary_ready() : adreno8_hwsched_ready()) ||
	    !adreno8_gpu_memory_ready() || adreno8_gpu_faulted())
		return false;
	return GMU_PWR_GX_UP(gpu_gmu_read(GMU_PWR_CLK_STATUS));
}

static bool
gpu_status_ok(void)
{
	uint32_t interrupt, cp;
	if (!gpu_accessible())
		return gpu_fail("power, HFI or translation check");
	interrupt = gpu_read(GEN8_RBBM_INT_0_STATUS);
	cp = gpu_read(GEN8_CP_INTERRUPT_STATUS_GLOBAL);
	if (gpu_state == GPU_FAILED)
		return false;
	if ((interrupt & A8_GPU_ERROR_MASK) || (cp & A8_CP_GLOBAL_MASK)) {
		printf("adreno8: GPU fault: RBBM_INT=%#x CP_GLOBAL=%#x\n", interrupt, cp);
		return gpu_fail("CP status");
	}
	return true;
}

static bool
gpu_wait_idle(const char *what)
{
	for (unsigned i = 0; i < 2000; i++) {
		if (!gpu_status_ok())
			return false;
		if (!(gpu_gmu_read(GMU(GEN8_GMUAO_GPU_CX_BUSY_STATUS)) & (1u << 23)))
			return gpu_status_ok();
		adreno8_sleep_us(100);
	}
	return gpu_fail(what);
}

static bool
gpu_raw(const uint32_t *words, uint32_t count, const char *what)
{
	printf("adreno8: GPU RAW %s: check power, translation and CP status\n", what);
	adreno8_hold(false);
	if (!gpu_status_ok())
		return false;
	printf("adreno8: GPU RAW %s: queue %u PM4 dwords through HFI 131\n", what, count);
	adreno8_hold(false);
	if (!count || !adreno8_hwsched_raw(words, count, what))
		return gpu_fail(what);
	printf("adreno8: GPU RAW %s: HFI ACK received; check idle and faults\n", what);
	adreno8_hold(false);
	return gpu_wait_idle(what);
}

static bool
gpu_load_ucode(const char *name, struct adreno8_memory **out)
{
	uint64_t size = 0;
	int disk = 0;
	const uint8_t *image = hakios_image_file("fw", name, &size, &disk);
	if (image == NULL || size < 8 || size > 0x1000000 || (size & 3))
		return gpu_fail("microcode file bounds");
	*out = adreno8_gpu_alloc(name, (uint32_t)size - 4, 4096, ADRENO8_MEM_READ | ADRENO8_MEM_EXEC);
	if (*out == NULL)
		return gpu_fail("microcode allocation");
	memcpy((void *)(*out)->memory, image + 4, (size_t)size - 4);
	if (!adreno8_memory_publish(*out, 0, (uint32_t)size - 4))
		return gpu_fail("microcode publication");
	printf("adreno8: GPU %s payload=%llu IOVA=%#x\n", name, size - 4, (*out)->gpu_address);
	return true;
}

bool
adreno8_gpu_prepare(void)
{
	if (gpu_state != GPU_COLD)
		return gpu_fail("duplicate preparation");
	if (!gpu_load_ucode("gen80000_sqe.fw", &sqe) || !gpu_load_ucode("gen80000_aqe.fw", &aqe))
		return false;
	powerup = adreno8_gpu_alloc("powerup list", 4096, 4096,
	    ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV);
	scratch = adreno8_gpu_alloc("CP scratch", 4096, 4096,
	    ADRENO8_MEM_READ | ADRENO8_MEM_WRITE | ADRENO8_MEM_PRIV);
	if (adreno8_direct_ring)
		ring = adreno8_gpu_alloc("direct ring", 32768, 4096, ADRENO8_MEM_READ | ADRENO8_MEM_PRIV);
	if (!powerup || !scratch || (adreno8_direct_ring && !ring))
		return gpu_fail("GPU buffer allocation");
	memset((void *)scratch->memory, 0, scratch->size);
	if (!adreno8_memory_publish(scratch, 0, scratch->size))
		return gpu_fail("scratch publication");
	gpu_state = GPU_PREPARED;
	return true;
}

static bool
gpu_oob_get(void)
{
	if (!(adreno8_direct_ring ? adreno8_ordinary_ready() : adreno8_hwsched_ready()) ||
	    !adreno8_gpu_memory_ready() || adreno8_gpu_faulted())
		return gpu_fail("OOB prerequisites");
	/* gen8_gmu_oob_set(oob_gpu): request bit 30, response bit 31. */
	adreno8_reset_required = true;
	oob_requested = true;
	gpu_gmu_write(GMU_HOST2GMU_SET, 1u << 30);
	adreno8_settle();
	for (unsigned i = 0; i < 1000; i++) {
		if (gpu_gmu_read(GMU_GMU2HOST_INFO) & (1u << 31)) {
			oob_acked = true;
			break;
		}
		adreno8_sleep_us(100);
	}
	gpu_gmu_write(GMU_GMU2HOST_CLR, 1u << 31);
	adreno8_settle();
	if (!oob_acked || !gpu_accessible())
		return gpu_fail("OOB acknowledgement or GX power");
	printf("adreno8: GPU OOB acknowledged and held until cold reset\n");
	return true;
}

static bool
gpu_noncontext(void)
{
	for (unsigned pipe = 0; pipe <= 5; pipe++) {
		if (adreno8_direct_ring && pipe == A8_PIPE_LPAC)
			continue;
		if (!gpu_aperture(pipe))
			return false;
		for (unsigned i = 0; i < A8_COUNT(a8_noncontext); i++)
			if (a8_noncontext[i].pipes & (1u << pipe))
				gpu_write(a8_noncontext[i].reg, a8_noncontext[i].value);
	}
	return gpu_aperture(0);
}

static bool
gpu_protect(void)
{
	uint32_t last = 0;
	for (unsigned pipe = 1; pipe <= 2; pipe++)
		if (!gpu_pipe_write(pipe, GEN8_CP_PROTECT_CNTL_PIPE, 0xffff000b))
			return false;
	if (!gpu_aperture(0))
		return false;
	for (unsigned i = 0; i < A8_COUNT(a8_protected); i++) {
		const struct a8_protect_span *p = &a8_protected[i];
		last = p->start | ((p->end - p->start) << 18) | (p->noaccess << 31);
		gpu_write(p->reg, last);
	}
	for (unsigned pipe = 1; pipe <= 2; pipe++)
		if (!gpu_pipe_write(pipe, GEN8_CP_PROTECT_REG_PIPE + 15, last))
			return false;
	return gpu_aperture(0);
}

static bool
gpu_hwcg(void)
{
	/* Preserve inherited clock-gating settings before CP_INIT. */
	if (adreno8_direct_ring)
		return true;
	gpu_write(GEN8_RBBM_CGC_0_PC, 0x702);
	for (unsigned i = 0; i < A8_COUNT(a8_ao_hwcg); i++)
		gpu_gmu_write(GMU(a8_ao_hwcg[i].reg), a8_ao_hwcg[i].value);
	gpu_write(GEN8_RBBM_CLOCK_CNTL_GLOBAL, 1);
	gpu_write(GEN8_RBBM_CGC_GLOBAL_LOAD_CMD, 1);
	gpu_write(GEN8_RBBM_CGC_P2S_TRIG_CMD, 1);
	adreno8_settle();
	for (unsigned i = 0; i < 3; i++) {
		adreno8_sleep_us(1);
		if (gpu_read(GEN8_RBBM_CGC_P2S_STATUS) & 1) {
			gpu_write(GEN8_RBBM_CLOCK_CNTL_GLOBAL, 0);
			return true;
		}
	}
	return gpu_fail("clock-gating TXDONE");
}

static bool
gpu_powerup_list(void)
{
	uint32_t *words = (uint32_t *)powerup->memory;
	uint32_t at = 4, dynamic = 0;
	uint32_t dynamic_count = 0;
	for (unsigned pipe = 1; pipe <= 7; pipe++)
		for (unsigned i = 0; i < A8_COUNT(a8_powerup_ext); i++)
			dynamic_count += !!(a8_powerup_ext[i].pipes & (1u << pipe));
	uint32_t needed = 4 + 2 * (A8_COUNT(a8_powerup_ifpc) + A8_COUNT(a8_powerup)) + 3 * dynamic_count;
	if (needed > powerup->size / 4 || A8_COUNT(a8_powerup_ifpc) > 255 || A8_COUNT(a8_powerup) > 255)
		return gpu_fail("powerup list capacity");
	memset(words, 0, powerup->size);
	if (!gpu_aperture(0))
		return false;
	for (unsigned i = 0; i < A8_COUNT(a8_powerup_ifpc); i++) {
		words[at++] = a8_powerup_ifpc[i];
		words[at++] = gpu_read(a8_powerup_ifpc[i]);
	}
	for (unsigned i = 0; i < A8_COUNT(a8_powerup); i++) {
		words[at++] = a8_powerup[i];
		words[at++] = gpu_read(a8_powerup[i]);
	}
	/* Catalog pipe support controls this list; LPAC dispatch stays disabled. */
	for (unsigned pipe = 1; pipe <= 7; pipe++) {
		if (!gpu_aperture(pipe))
			return false;
		for (unsigned i = 0; i < A8_COUNT(a8_powerup_ext); i++) {
			if (!(a8_powerup_ext[i].pipes & (1u << pipe)))
				continue;
			words[at++] = pipe << 12;
			words[at++] = a8_powerup_ext[i].reg;
			words[at++] = gpu_read(a8_powerup_ext[i].reg);
			dynamic++;
		}
	}
	if (!gpu_aperture(0))
		return false;
	words[3] = A8_COUNT(a8_powerup_ifpc) | (A8_COUNT(a8_powerup) << 8) | (dynamic << 16);
	if (at != needed || dynamic != dynamic_count || !adreno8_memory_publish(powerup, 0, at * 4))
		return gpu_fail("powerup list publication");
	printf("adreno8: GPU powerup list IOVA=%#x bytes=%u IFPC=%u preemption=%u dynamic=%u\n",
	    powerup->gpu_address, at * 4, (unsigned)A8_COUNT(a8_powerup_ifpc), (unsigned)A8_COUNT(a8_powerup), dynamic);
	return true;
}

static bool
gpu_register_config(void)
{
	if (!gpu_aperture(0))
		return false;
	/* a8xx hw_init releases CX and GX halts before configuring the core. */
	if (adreno8_direct_ring) {
		const uint32_t release[] = { GEN8_GBIF_HALT, GEN8_RBBM_GBIF_HALT };
		for (unsigned i = 0; i < A8_COUNT(release); i++) {
			gpu_write(release[i], 0);
			adreno8_settle();
			if (gpu_read(release[i]) != 0)
				return gpu_fail("ordinary GBIF halt release readback");
		}
	}
	/* Polling owns error handling during this cold-reset experiment. */
	gpu_write(GEN8_RBBM_INT_0_MASK, 0);
	gpu_write(GEN8_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	/* 12 MiB GMEM aligned to the catalog's 64 MiB UCHE base alignment. */
	gpu_write(GEN8_UCHE_CCHE_GC_GMEM_RANGE_MIN_LO, 0x04000000);
	gpu_write(GEN8_UCHE_CCHE_GC_GMEM_RANGE_MIN_HI, 0);
	gpu_write(GEN8_SP_HLSQ_GC_GMEM_RANGE_MIN_LO, 0x04000000);
	gpu_write(GEN8_SP_HLSQ_GC_GMEM_RANGE_MIN_HI, 0);
	gpu_write(GEN8_UCHE_TRAP_BASE_LO, 0xfffff000);
	gpu_write(GEN8_UCHE_TRAP_BASE_HI, 0x1ffff);
	gpu_write(GEN8_UCHE_WRITE_THRU_BASE_LO, 0xfffff000);
	gpu_write(GEN8_UCHE_WRITE_THRU_BASE_HI, 0x1ffff);
	gpu_gmu_write(GMU(GEN8_GMUCX_POWER_COUNTER_SELECT_XOCLK_1), 4);
	/* sun-gpu.dtsi: UBWC 5, min-access-length 32; gen8_0_0 highest_bank_bit 16. */
	if (!gpu_pipe_write(2, GEN8_GRAS_NC_MODE_CNTL, 0x60) ||
	    !gpu_pipe_write(1, GEN8_GRAS_NC_MODE_CNTL, 0x60) ||
	    !gpu_pipe_write(1, GEN8_RB_CCU_NC_MODE_CNTL, 6) ||
	    !gpu_pipe_write(1, GEN8_RB_CMP_NC_MODE_CNTL, 0x20006) || !gpu_aperture(0))
		return false;
	gpu_write(GEN8_SP_NC_MODE_CNTL, 0x26);
	gpu_write(GEN8_TPL1_NC_MODE_CNTL, 6);
	for (unsigned i = 0; i < A8_COUNT(a8_bicubic); i++)
		gpu_write(a8_bicubic[i].reg, a8_bicubic[i].value);
	gpu_write(GEN8_UCHE_CLIENT_PF, 0x81);
	if (!gpu_noncontext())
		return false;
	gpu_write(GEN8_RBBM_INTERFACE_HANG_INT_CNTL, 0x40cfffff);
	gpu_write(GEN8_RBBM_SLICE_INTERFACE_HANG_INT_CNTL, 1u << 30);
	gpu_gmu_write(GMU(GEN8_GMUAO_GPU_CX_BUSY_MASK), 0xff000000);
	uint32_t counter = gpu_gmu_read(GMU(GEN8_GMUCX_POWER_COUNTER_SELECT_XOCLK_0));
	gpu_gmu_write(GMU(GEN8_GMUCX_POWER_COUNTER_SELECT_XOCLK_0), (counter & ~0xffu) | 0x20);
	gpu_gmu_write(GMU(GEN8_GMUCX_POWER_COUNTER_ENABLE), 1);
	if (!gpu_protect())
		return false;
	/* No LLCC slices are allocated; vendor LLCC helpers return in this case. */
	for (unsigned pipe = 1; pipe <= 7; pipe++) {
		if (adreno8_direct_ring && pipe == A8_PIPE_LPAC)
			continue;
		if (!gpu_pipe_write(pipe, GEN8_CP_APRIV_CNTL_PIPE, pipe == 1 ? 0x6f : 0xf))
			return false;
		gpu_write(GEN8_CP_INTERRUPT_STATUS_MASK_PIPE, A8_CP_SW_MASK);
		gpu_write(GEN8_CP_HW_FAULT_STATUS_MASK_PIPE, A8_CP_HW_MASK);
	}
	if (!gpu_aperture(0))
		return false;
	gpu_write(GEN8_CP_INTERRUPT_STATUS_MASK_GLOBAL, A8_CP_GLOBAL_MASK);
	gpu_write(GEN8_RBBM_SECVID_TSB_CNTL, 0);
	gpu_write(GEN8_RBBM_SECVID_TSB_TRUSTED_BASE_LO, adreno8_direct_ring ? 0 : ADRENO8_SECURE_IOVA_START);
	gpu_write(GEN8_RBBM_SECVID_TSB_TRUSTED_BASE_HI, 0);
	gpu_write(GEN8_RBBM_SECVID_TSB_TRUSTED_SIZE, adreno8_direct_ring ? 0 : 0xfffff000);
	if (!gpu_hwcg() || (!adreno8_direct_ring && !gpu_powerup_list()))
		return false;
	adreno8_settle();
	return gpu_status_ok();
}

static bool
gpu_ucode_base(uint32_t reg, const struct adreno8_memory *memory)
{
	gpu_write(reg, memory->gpu_address);
	gpu_write(reg + 1, 0);
	adreno8_settle();
	if (gpu_read(reg) != memory->gpu_address || gpu_read(reg + 1) != 0)
		return gpu_fail("microcode base readback");
	return true;
}

/* Follow a6xx_fenced_write's WPTR/barrier/bit-0 check. The 200 failed attempts
 * have a 2 ms delay budget plus MMIO time; logging and diagnostic pauses stay outside. */
static bool
gpu_ring_wptr(uint32_t value)
{
	if (gpu_state == GPU_FAILED || !adreno8_gpu.base || !adreno8_gmu.base ||
	    GEN8_CP_RB_WPTR_GC >= adreno8_gpu.size / 4)
		return gpu_fail("direct ring WPTR aperture");
	for (unsigned i = 0; i < 200; i++) {
		adreno8_settle();
		adreno8_wr(adreno8_gpu.base + 4u * GEN8_CP_RB_WPTR_GC, value);
		adreno8_settle();
		uint32_t fence = gpu_gmu_read(GMU_AHB_FENCE_STATUS);
		if (fence == 0xffffffffu)
			return gpu_fail("direct ring fence read");
		if (!(fence & 1u))
			return true;
		adreno8_sleep_us(10);
	}
	return gpu_fail("direct ring fenced WPTR timeout");
}

/* Arm IHI 0062D.c GR0: sGFAR 0x40/44, sGFSR 0x48, sGFSYNR0/1 0x50/54.
 * Preserve raw details; a fault arriving during the sample also stops this probe. */
static bool
gpu_global_fault_sample(const char *phase)
{
	if (!adreno8_smmu.base || adreno8_smmu.size < 0x58)
		return false;
	uint32_t first = adreno8_rd(adreno8_smmu.base + SMMU_SGFSR);
	uint32_t low = adreno8_rd(adreno8_smmu.base + 0x40);
	uint32_t high = adreno8_rd(adreno8_smmu.base + 0x44);
	uint32_t syn0 = adreno8_rd(adreno8_smmu.base + 0x50);
	uint32_t syn1 = adreno8_rd(adreno8_smmu.base + 0x54);
	uint32_t last = adreno8_rd(adreno8_smmu.base + SMMU_SGFSR);
	char line[192];
	snprintf(line, sizeof(line), "adreno8: ordinary SQE global fault %s: fsr=%#x/%#x far=%#x:%08x synr=%#x/%#x\n",
	    phase, first, last, high, low, syn0, syn1);
	loader_log_line(line);
	return first == 0 && last == 0;
}

/* Read-only state after an unconsumed CP_INIT, following gen8_spin_idle_debug and
 * Linux a8xx_fault_detect_irq. SQE_STAT indices are recorded raw; mainline only
 * names index 1 (faulting opcode). */
static void
gpu_direct_ring_timeout_dump(void)
{
	static const uint32_t global[] = { GEN8_RBBM_STATUS1, GEN8_RBBM_GFX_STATUS,
		GEN8_RBBM_GFX_BR_STATUS, GEN8_RBBM_GFX_BV_STATUS, GEN8_RBBM_MISC_STATUS,
		GEN8_CP_CP2GMU_STATUS, GEN8_CP_RL_ERROR_DETAILS_0, GEN8_CP_RL_ERROR_DETAILS_1,
		GEN8_CP_SQE_INSTR_BASE_LO, GEN8_CP_RB_BASE_LO_GC, GEN8_CP_RB_CNTL_GC };
	static const uint32_t piped[] = { GEN8_CP_HW_FAULT_STATUS_PIPE, GEN8_CP_INTERRUPT_STATUS_PIPE,
		GEN8_CP_PIPE_STATUS_PIPE, GEN8_CP_SQE_STATUS_PIPE, GEN8_CP_PROTECT_STATUS_PIPE,
		GEN8_CP_SQE_ICACHE_CNTL_PIPE };

	for (unsigned i = 0; i < A8_COUNT(global); i++)
		printf("adreno8: CP_INIT timeout reg %#x = %#x\n", global[i], gpu_read(global[i]));
	if (adreno8_memory_acquire(scratch, 0, 16))
		printf("adreno8: CP_INIT timeout shadow BR=%#x BV=%#x\n",
		    *(volatile uint32_t *)scratch->memory, *(volatile uint32_t *)(scratch->memory + 8));
	for (uint32_t pipe = A8_PIPE_BR; pipe <= A8_PIPE_BV; pipe++) {
		if (!gpu_aperture(pipe))
			return;
		for (unsigned i = 0; i < A8_COUNT(piped); i++)
			printf("adreno8: CP_INIT timeout pipe %u reg %#x = %#x\n", pipe, piped[i], gpu_read(piped[i]));
		for (uint32_t index = 0; index < 16; index++) {
			gpu_write(GEN8_CP_SQE_STAT_ADDR_PIPE, index);
			printf("adreno8: CP_INIT timeout pipe %u SQE_STAT[%u] = %#x\n",
			    pipe, index, gpu_read(GEN8_CP_SQE_STAT_DATA_PIPE));
		}
	}
	(void)gpu_aperture(0);
}

/* Linux a8xx_hw_init/a8xx_cp_init at efbda69d92e6 use a zeroed powerup page,
 * BR/BV RPTR shadows and a 32 KiB ring before enabling SQE. The setup probes
 * stop before CP_INIT and retain allocations and OOB until cold reset. */
static bool
gpu_direct_ring_init(uint32_t raw_mode)
{
	if (!ring || ring->size != 32768 || scratch->size < 16 || powerup->size != 4096)
		return gpu_fail("direct ring buffer bounds");
	memset((void *)ring->memory, 0, ring->size);
	memset((void *)scratch->memory, 0, scratch->size);
	memset((void *)powerup->memory, 0, powerup->size);
	if (!adreno8_memory_publish(ring, 0, ring->size) ||
	    !adreno8_memory_publish(scratch, 0, scratch->size) ||
	    !adreno8_memory_publish(powerup, 0, powerup->size))
		return gpu_fail("direct ring initial publication");
	if (gpu_read(GEN8_CP_RB_RPTR_BR) != 0 || gpu_read(GEN8_CP_RB_WPTR_GC) != 0)
		return gpu_fail("direct ring requires cold indices");
	const struct a8_reg_pair setup[] = {
		{ GEN8_CP_RB_BASE_LO_GC, ring->gpu_address }, { GEN8_CP_RB_BASE_HI_GC, 0 },
		{ GEN8_CP_RB_CNTL_GC, 0x20c },
		{ GEN8_CP_RB_RPTR_ADDR_LO_BR, scratch->gpu_address }, { GEN8_CP_RB_RPTR_ADDR_HI_BR, 0 },
		{ GEN8_CP_RB_RPTR_ADDR_LO_BV, scratch->gpu_address + 8 }, { GEN8_CP_RB_RPTR_ADDR_HI_BV, 0 }
	};
	for (unsigned i = 0; i < A8_COUNT(setup); i++) {
		gpu_write(setup[i].reg, setup[i].value);
		adreno8_settle();
		if (gpu_read(setup[i].reg) != setup[i].value)
			return gpu_fail("direct ring register readback");
	}
	if (raw_mode) {
		if (raw_mode == 2) {
			uint32_t inherited = gpu_read(GEN8_CP_SQE_CNTL);
			printf("adreno8: ordinary SQE zero-value control inherited %#x = %#x\n",
			    adreno8_gpu.phys + 4u * GEN8_CP_SQE_CNTL, inherited);
			if (inherited != 0)
				return gpu_fail("ordinary SQE zero-value control requires inherited 0");
			/* Vendor adreno_hwsched.c writes CP_ME_CNTL=0 during active/GX-on stall recovery. */
			gpu_write(GEN8_CP_SQE_CNTL, 0);
			adreno8_settle();
			loader_log_line("adreno8: ordinary SQE zero-value control DSB returned\n");
			if (gpu_read(GEN8_CP_SQE_CNTL) != 0)
				return gpu_fail("ordinary SQE zero-value control readback");
		}
		/* Mainline reads HALT after writes and polls ACK during shutdown. */
		const uint32_t halt[] = { GEN8_GBIF_HALT, GEN8_GBIF_HALT_ACK,
			GEN8_RBBM_GBIF_HALT, GEN8_RBBM_GBIF_HALT_ACK };
		for (unsigned i = 0; i < A8_COUNT(halt); i++) {
			uint32_t value = gpu_read(halt[i]);
			printf("adreno8: ordinary setup-only halt sample %#x = %#x\n", adreno8_gpu.phys + 4u * halt[i], value);
			if (value == 0xffffffffu)
				return gpu_fail("ordinary setup-only halt read");
		}
		printf("adreno8: ordinary setup-only: ring registers verified; %s; CP_INIT remains unissued\n",
		    raw_mode == 2 ? "SQE zero-value control readback verified" : "SQE enable remains unissued");
		for (unsigned i = 0; i < 100; i++) {
			adreno8_sleep_us(100000);
			if (!adreno8_hfi_poll() || !gpu_status_ok())
				return gpu_fail("ordinary setup-only HFI or GPU fault");
		}
		printf("adreno8: ordinary setup-only remained healthy for 10 seconds through the final check; %s\n",
		    raw_mode == 2 ? "SQE zero-value write was issued and zero readback verified before this interval" : "SQE was not started by this probe");
		return gpu_fail("ordinary setup-only diagnostic stop");
	}
	if (!gpu_global_fault_sample("before store"))
		return gpu_fail("global fault before ordinary SQE store");
	gpu_write(GEN8_CP_SQE_CNTL, 1);
	adreno8_settle();
	loader_log_line("adreno8: ordinary SQE start DSB returned\n");
	if (!gpu_global_fault_sample("after DSB"))
		return gpu_fail("global fault after ordinary SQE DSB");
	if (gpu_read(GEN8_CP_SQE_CNTL) != 1)
		return gpu_fail("direct ring SQE enable readback");
	uint32_t count = adreno8_pm4_cp_init((uint32_t *)ring->memory, ring->size / 4, powerup->gpu_address);
	if (count != 10 || !adreno8_memory_publish(ring, 0, count * 4))
		return gpu_fail("direct ring CP_INIT publication");
	printf("adreno8: direct ring CP_INIT published: ring=%#x shadow=%#x powerup=%#x dwords=%u\n",
	    ring->gpu_address, scratch->gpu_address, powerup->gpu_address, count);
	if (!gpu_ring_wptr(count))
		return false;
	for (unsigned i = 0; i < 2000; i++) {
		if (!adreno8_hfi_poll() || !gpu_status_ok())
			return gpu_fail("direct ring HFI, GPU or GMU fault");
		uint32_t rptr = gpu_read(GEN8_CP_RB_RPTR_BR), bv = gpu_read(GEN8_CP_RB_RPTR_BV);
		if (!adreno8_memory_acquire(scratch, 0, 16))
			return gpu_fail("direct ring shadow acquire");
		uint32_t br_shadow = *(volatile uint32_t *)scratch->memory;
		uint32_t bv_shadow = *(volatile uint32_t *)(scratch->memory + 8);
		if (rptr >= ring->size / 4 || bv >= ring->size / 4 ||
		    br_shadow >= ring->size / 4 || bv_shadow >= ring->size / 4)
			return gpu_fail("direct ring RPTR bounds");
		if (rptr == count && br_shadow == count &&
		    !(gpu_gmu_read(GMU(GEN8_GMUAO_GPU_CX_BUSY_STATUS)) & (1u << 23)) &&
		    !(gpu_read(GEN8_RBBM_STATUS) & ~1u)) {
			if (!gpu_status_ok())
				return false;
			printf("adreno8: direct ring CP_INIT consumed and idle: BR=%u shadow=%u BV=%u shadow=%u\n",
			    rptr, br_shadow, bv, bv_shadow);
			return gpu_fail("direct ring diagnostic complete; stop");
		}
		adreno8_sleep_us(100);
	}
	(void)gpu_global_fault_sample("completion timeout");
	printf("adreno8: direct ring CP_INIT timeout: WPTR=%#x RBBM_STATUS=%#x\n",
	    gpu_read(GEN8_CP_RB_WPTR_GC), gpu_read(GEN8_RBBM_STATUS));
	gpu_direct_ring_timeout_dump();
	return gpu_fail("direct ring CP_INIT completion timeout");
}

bool
adreno8_gpu_cp_init(void)
{
	uint32_t words[10], raw_nop = 0, empty_powerup = 0;
	if (gpu_state != GPU_PREPARED)
		return gpu_fail("CP initialization order");
	PE_parse_boot_argn("adreno8_raw_nop", &raw_nop, sizeof(raw_nop));
	PE_parse_boot_argn("adreno8_empty_powerup", &empty_powerup, sizeof(empty_powerup));
	if (raw_nop > 3)
		return gpu_fail("unknown RAW NOP diagnostic");
	if (empty_powerup && (empty_powerup != 1 || adreno8_step != 17 || adreno8_newest_step != 17 ||
	    !adreno8_gpu8_keep || adreno8_gpu8_return || adreno8_skip_steps != 0 || raw_nop != 0))
		return gpu_fail("empty powerup diagnostic prerequisites");
	if (adreno8_direct_ring && (adreno8_step != 17 || adreno8_newest_step != 17 ||
	    !adreno8_gpu8_keep || adreno8_gpu8_return || adreno8_skip_steps != 0 ||
	    (raw_nop != 0 && raw_nop != 2 && raw_nop != 3) || empty_powerup))
		return gpu_fail("direct ring diagnostic prerequisites");
	if (adreno8_direct_ring &&
	    (!adreno8_translation_walk(&adreno8_gpu_space, sqe->gpu_address, sqe->memory, sqe->size,
	        ADRENO8_MEM_READ | ADRENO8_MEM_EXEC) ||
	    !adreno8_translation_walk(&adreno8_gpu_space, aqe->gpu_address, aqe->memory, aqe->size,
	        ADRENO8_MEM_READ | ADRENO8_MEM_EXEC)))
		return gpu_fail("microcode object mapping");
	if (!gpu_oob_get() || !gpu_register_config() ||
	    !gpu_ucode_base(GEN8_CP_SQE_INSTR_BASE_LO, sqe) ||
	    !gpu_ucode_base(GEN8_CP_AQE_INSTR_BASE_LO_0, aqe))
		return false;
	if (adreno8_direct_ring)
		return gpu_direct_ring_init(raw_nop);
	/* Separate delayed failure after setup from failure after publishing RAW. */
	if (raw_nop == 3) {
		for (unsigned i = 0; i < 100; i++) {
			if (!gpu_status_ok())
				return false;
			adreno8_sleep_us(100000);
		}
		printf("adreno8: CP setup-only diagnostic remained accessible for 10 seconds; no RAW published\n");
		return gpu_fail("CP setup-only diagnostic stop");
	}
	/* The vendor first sends CP_INIT. This diagnostic stops after one NOP,
	 * or five NOPs with the same message length as CP_INIT; CP_READY is never set. */
	if (raw_nop) {
		uint32_t count = raw_nop == 1 ? 2 : 10;
		for (uint32_t i = 0; i < count; i += 2) {
			words[i] = adreno8_pm4_type7(0x10, 1);
			words[i + 1] = 0;
		}
		if (!gpu_raw(words, count, "pre-init NOP"))
			return false;
		printf("adreno8: RAW NOP diagnostic acknowledged and idle (%u PM4 dwords)\n", count);
		return gpu_fail("RAW NOP diagnostic stop");
	}
	/* Mainline cold boot sends CP_INIT before patching its zeroed powerup page.
	 * This diagnostic tests that input with HWSCHED; firmware acceptance needs a device run. */
	if (empty_powerup) {
		memset((void *)powerup->memory, 0, powerup->size);
		if (!adreno8_memory_publish(powerup, 0, powerup->size))
			return gpu_fail("empty powerup publication");
		printf("adreno8: empty powerup page published: IOVA=%#x bytes=%u\n", powerup->gpu_address, powerup->size);
	}
	uint32_t count = adreno8_pm4_cp_init(words, A8_COUNT(words), powerup->gpu_address);
	if (!gpu_raw(words, count, "CP initialization"))
		return false;
	if (empty_powerup)
		return gpu_fail("empty powerup diagnostic acknowledged and idle; stop");
	gpu_state = GPU_CP_READY;
	printf("adreno8: GPU CP initialization acknowledged and idle; zap authentication is next\n");
	return true;
}

bool
adreno8_gpu_cp_ready(void)
{
	return gpu_state == GPU_CP_READY && gpu_accessible();
}

bool
adreno8_gpu_secure_exit(void)
{
	uint32_t words[2];
	if (gpu_state != GPU_CP_READY)
		return gpu_fail("secure-mode transition order");
	uint32_t count = adreno8_pm4_secure_exit(words, A8_COUNT(words));
	if (!gpu_raw(words, count, "secure-mode transition"))
		return false;
	gpu_state = GPU_UNSECURE;
	return true;
}

bool
adreno8_gpu_scratch_test(void)
{
	uint32_t words[6];
	const uint32_t expected = 0x600dc0de;
	if (gpu_state != GPU_UNSECURE)
		return gpu_fail("scratch command order");
	uint32_t count = adreno8_pm4_scratch(words, A8_COUNT(words), scratch->gpu_address, expected);
	if (!gpu_raw(words, count, "scratch memory write") ||
	    !adreno8_memory_acquire(scratch, 0, scratch->size))
		return gpu_fail("scratch completion");
	volatile uint32_t *data = (volatile uint32_t *)scratch->memory;
	if (data[0] != expected) {
		printf("adreno8: GPU scratch actual=%#x expected=%#x\n", data[0], expected);
		return gpu_fail("scratch readback");
	}
	for (unsigned i = 1; i < scratch->size / 4; i++)
		if (data[i] != 0)
			return gpu_fail("scratch bounds");
	if (!gpu_status_ok())
		return false;
	gpu_state = GPU_TESTED;
	printf("adreno8: GPU CP wrote %#x to scratch; idle and SMMU checks passed; OOB and resources held until cold reset\n", expected);
	return true;
}

bool
adreno8_gpu_commands_ready(void)
{
	return gpu_state == GPU_TESTED && gpu_status_ok();
}

bool
adreno8_gpu_wait_idle(const char *what)
{
	if (gpu_state != GPU_TESTED) {
		printf("adreno8: GPU %s idle wait refused in state %u\n", what, gpu_state);
		return false;
	}
	return gpu_wait_idle(what);
}
