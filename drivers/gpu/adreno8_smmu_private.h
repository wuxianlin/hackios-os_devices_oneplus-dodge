/* Register view shared by the SMMU mapper and secure translation setup. */
#ifndef _DEV_ADRENO8_SMMU_PRIVATE_H
#define _DEV_ADRENO8_SMMU_PRIVATE_H
#include "adreno8_memory_private.h"

extern struct adreno8_block adreno8_smmu;
extern uint32_t adreno8_smmu_page, adreno8_smmu_pages;

/* Arm IHI 0062D.c 9.6.21/22/29; fixed MiCode arm-smmu.h uses these offsets. */
#define SMMU_TLBIALLNSNH         0x68
#define SMMU_TLBGSYNC            0x70
#define SMMU_TLBGSTATUS          0x74

#define SMMU_S2CR_FAULT          (2u << 16)
#define SMMU_S2CR_SHCFG_NSH      (3u << 8)
#define SMMU_CB_ACTLR            0x04
/* Linux arm-smmu-qcom.c at efbda69d92e6: PREFETCH_DEEP | CPRE | CMTLB. */
#define SMMU_GPU_ACTLR           0x303u
#define SMMU_TCR2_TBI            (1u << 5 | 1u << 6)
#define SMMU_PTE_XN              (3ULL << 53)
#define SMMU_PTE_REQUIRED        (3ULL | 1ULL << 10 | 1ULL << 11)
#define SMMU_PTE_VARIABLE        (3ULL << 6 | 3ULL << 8 | SMMU_PTE_XN)

/* io-pgtable-arm.c at d50b305 uses bit 55 as ARM_LPAE_PTE_SW_SYNC.
 * Hardware ignores this software bit. In CB0 it records external ownership;
 * an unmapped reservation is exactly this value, with the Valid bit clear. */
#define ADRENO8_PTE_EXTERNAL     (1ULL << 55)

uint64_t *adreno8_space_entry(struct adreno8_gpu_space *, uint32_t address);
bool adreno8_space_prepare_page(struct adreno8_gpu_space *, uint32_t address);
bool adreno8_space_publish(struct adreno8_gpu_space *);
bool adreno8_space_publish_range(struct adreno8_gpu_space *, uint64_t address, uint64_t size);

static inline uint64_t
adreno8_space_attributes(uint32_t prot)
{
	if (!(prot & ADRENO8_MEM_READ) || (prot & ~15u)) return 0;
	uint64_t attributes = SMMU_PTE_REQUIRED | 2ULL << 8;
	if (!(prot & ADRENO8_MEM_WRITE)) attributes |= 1ULL << 7;
	if (!(prot & ADRENO8_MEM_PRIV)) attributes |= 1ULL << 6;
	if (!(prot & ADRENO8_MEM_EXEC)) attributes |= SMMU_PTE_XN;
	return attributes;
}

/* Arm IHI 0062D.c: CFCFG selects stall fault handling; CFIE remains clear so
 * fault registers can be polled without enabling an unhandled interrupt. */
static inline uint32_t
adreno8_smmu_sctlr(uint32_t context)
{
	return SCTLR_ON | (adreno8_gpu_stall && (context == 0 || context == SECURE_CONTEXT) ? SCTLR_CFCFG : 0);
}

#endif
