/* gen8.0.0 HWSCHED startup and memory allocation contract. */
#ifndef _DEV_ADRENO8_HWSCHED_PRIVATE_H
#define _DEV_ADRENO8_HWSCHED_PRIVATE_H

#define ADRENO8_HFI_MEM_GFX        (1u << 0)
#define ADRENO8_HFI_MEM_GFX_PRIV   (1u << 1)
#define ADRENO8_HFI_MEM_GFX_WRITE  (1u << 2)
#define ADRENO8_HFI_MEM_GMU        (1u << 3)
#define ADRENO8_HFI_MEM_GMU_PRIV   (1u << 4)
#define ADRENO8_HFI_MEM_GMU_WRITE  (1u << 5)
#define ADRENO8_HFI_MEM_BUFFERABLE (1u << 6)
#define ADRENO8_HFI_MEM_CACHEABLE  (1u << 7)
#define ADRENO8_HFI_MEM_HOST       (1u << 8)
#define ADRENO8_HFI_MEM_HOST_INIT  (1u << 9)
#define ADRENO8_HFI_MEM_SECURE     (1u << 12)

struct adreno8_hfi_mem_request {
	uint32_t flags, kind, handle, size, align;
};

struct adreno8_hfi_mem_result {
	uint64_t gpu_address;
	uint32_t gmu_address, size;
	/* True only after the secure owner completes assignment and secure GPU mapping. */
	bool secure;
};

/* The allocation owner retains mappings until cold reset. Repeated (kind, handle)
 * requests must match all fields and return the same allocation. size is allocated
 * bytes; align encodes GMU VA and mapping-size powers of two in its low two bytes. */
bool adreno8_hwsched_alloc(const struct adreno8_hfi_mem_request *request,
    struct adreno8_hfi_mem_result *result);

/* Mode 4 closes allocations on transport/start failure and retains all pages. */
void adreno8_hwsched_memory_failed(void);

extern bool adreno8_hfi_tables_sent;
bool adreno8_hfi_send(uint32_t id, const uint32_t *payload, uint32_t count, const char *what);
/* Drain response/debug queues and stop allocations on transport or device faults. */
bool adreno8_hfi_poll(void);
void adreno8_hfi_worked(uint64_t *microseconds, uint32_t *count, uint32_t by[7]);

/* Produce an ACK payload, including the echoed request header, in at most ten words. */
bool adreno8_hwsched_request(const uint32_t *request, uint32_t words,
    uint32_t *reply, uint32_t *reply_words);
bool adreno8_hwsched_start(void);
bool adreno8_hwsched_ready(void);
/* The caller holds OOB_GPU and validates CP idle after the firmware ACK. */
bool adreno8_hwsched_raw(const uint32_t *pm4, uint32_t dwords, const char *name);

#endif
