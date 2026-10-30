/* Legacy GPU implementations called only by the fixed-platform C ABI dispatcher. */
#ifndef _DEV_REDWOOD_GPU_PRIVATE_H
#define _DEV_REDWOOD_GPU_PRIVATE_H

extern uint32_t gpu_log;

bool legacy_gpu_ready(void);
bool legacy_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t legacy_gpu_address(uint64_t size);
void legacy_gpu_address_free(uint64_t address, uint64_t size);
void legacy_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes,
    unsigned *most, uint64_t *lost, uint32_t *refused);
bool legacy_gpu_map(uint64_t address, uint64_t phys, bool read_only);
bool legacy_gpu_mapped(void);
bool legacy_gpu_unmap(uint64_t address, uint64_t size);
void legacy_gpu_to_memory(void *start, uint64_t size);
uint32_t legacy_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool legacy_gpu_done(uint32_t fence);
uint32_t legacy_gpu_last(void);
void legacy_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
void legacy_gpu_report(const char *why);
bool legacy_gpu_recover(void);
void legacy_gpu_stop(void);

#endif
