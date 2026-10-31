/* External physical BO pages in the existing non-secure GPU address space. */
#ifndef _DEV_ADRENO8_BO_PRIVATE_H
#define _DEV_ADRENO8_BO_PRIVATE_H
#include "adreno8_memory_private.h"

/* Stage 23 explicitly enables this path after the standard context self-test.
 * The caller serializes these operations with every GPU submission. */
bool adreno8_bo_enable(void);
bool adreno8_bo_ready(void);
/* Close all BO admission without releasing pages or IOVAs. */
void adreno8_bo_stop(void);
uint64_t adreno8_bo_reserve(uint64_t size);
/* The existing GEM interface identifies only read-only versus writable BOs.
 * Mesa uses it for shader, IB and data pages; all retain READ/EXEC permission. */
bool adreno8_bo_map(uint64_t address, uint64_t phys, bool read_only);
bool adreno8_bo_commit(void);
/* Use the address returned by reserve and the original requested size. Unmap confirms latest
 * EOP, GPU idle and TLB completion; only then may the caller release its pages.
 * Release returns the unmapped IOVA to the same page-table occupancy record.
 * Failures after enable close subsequent BO operations; any existing reservation,
 * mapping and backing pages remain owned until cold reset. */
bool adreno8_bo_unmap(uint64_t address, uint64_t size);
bool adreno8_bo_release(uint64_t address, uint64_t size);

#endif
