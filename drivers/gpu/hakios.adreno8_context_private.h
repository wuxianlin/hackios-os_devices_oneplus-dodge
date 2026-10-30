/* gen8 HWSCHED context submission; callers serialize all operations. */
#ifndef _DEV_ADRENO8_CONTEXT_PRIVATE_H
#define _DEV_ADRENO8_CONTEXT_PRIVATE_H

#define ADRENO8_CONTEXT_MAX_IBS 82u

/* OOB and command initialization stay owned by adreno8_gpu.c. */
bool adreno8_gpu_commands_ready(void);
bool adreno8_gpu_wait_idle(const char *what);

/* Pages and aliases remain allocated until cold reset, including after close.
 * A confirmed close permits registration again; any failure closes this boot. */
bool adreno8_context_prepare(void);
/* Addresses use the current 32-bit GPU VA space; lengths are dwords. There is
 * at most one in-flight submission. Zero reports failure and is never a fence. */
uint32_t adreno8_context_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
/* Completion requires EOP memory and healthy transport/device state. */
bool adreno8_context_done(uint32_t timestamp);
uint32_t adreno8_context_last(void);
uint32_t adreno8_context_depth(void);
bool adreno8_context_close(void);
bool adreno8_context_test(void);
/* Require the completed stage-22 self-test, latest EOP and GPU idle. The caller
 * keeps submissions serialized until its page-table operation has finished. */
bool adreno8_context_wait_idle(const char *what);

#endif
