/* Persistent buffers shared by the GPU, GMU and the host during bring-up. */
#ifndef _DEV_ADRENO8_MEMORY_PRIVATE_H
#define _DEV_ADRENO8_MEMORY_PRIVATE_H
#include "adreno8_private.h"

#define ADRENO8_MEM_READ  1u
#define ADRENO8_MEM_WRITE 2u
#define ADRENO8_MEM_EXEC  4u
#define ADRENO8_MEM_PRIV  8u

#define ADRENO8_GPU_IOVA_START 0x10000000u
#define ADRENO8_GPU_IOVA_END   0xe0000000u

/* kgsl_iommu.h at d50b305f7da9: KGSL_IOMMU_SECURE_BASE32 starts the 32-bit secure
 * subset of the SECVID trusted range. The allocator's uint32_t exclusive
 * end leaves the final 4 KiB page unused. */
#define ADRENO8_SECURE_IOVA_START 0xf0000000u
#define ADRENO8_SECURE_IOVA_END   0xfffff000u

struct adreno8_memory {
	const char *name;
	vm_offset_t memory;
	uint32_t size, gpu_address, gmu_address;
};

/* Calls are serialized by the boot/HFI caller. Published mappings last until reset. */
extern struct adreno8_gpu_space adreno8_gpu_space;
bool adreno8_space_find_range(struct adreno8_gpu_space *, uint32_t start, uint32_t end,
    uint32_t size, uint32_t alignment, uint32_t *address);
bool adreno8_space_map_region(struct adreno8_gpu_space *, uint32_t address,
    vm_offset_t memory, uint32_t size, uint32_t prot);
/* Check an active non-secure space without changing registers or issuing ATOS. */
bool adreno8_space_check(struct adreno8_gpu_space *, uint32_t stream, uint32_t match);
struct adreno8_memory *adreno8_gpu_alloc(const char *name, uint32_t size,
    uint32_t alignment, uint32_t prot);
struct adreno8_memory *adreno8_gmu_alloc(const char *name, uint32_t size,
    uint32_t alignment, uint32_t prot);
bool adreno8_gpu_memory_enable(void);
/* Read back the existing CB0 configuration. Failure closes allocation admission. */
bool adreno8_gpu_memory_verify(void);
bool adreno8_gpu_memory_ready(void);
bool adreno8_gpu_faulted(void);
bool adreno8_memory_retained(void);
/* CPU writes require publish before device access. Acquire requires clean CPU lines;
 * the CPU and device must not write the same cache line concurrently. */
bool adreno8_memory_publish(const struct adreno8_memory *, uint32_t offset, uint32_t size);
bool adreno8_memory_acquire(const struct adreno8_memory *, uint32_t offset, uint32_t size);

#endif
