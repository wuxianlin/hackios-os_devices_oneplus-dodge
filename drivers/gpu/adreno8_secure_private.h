/* MP_ASSIGN ownership transfer backing the HFI kind-5 secure allocation. */
#ifndef _DEV_ADRENO8_SECURE_PRIVATE_H
#define _DEV_ADRENO8_SECURE_PRIVATE_H
#include "adreno8_memory_private.h"

/* True after any failed or ambiguous MP_ASSIGN batch. Assigned pages stay where
 * they are and no further secure assignment runs until a cold reset. */
bool adreno8_secure_lost(void);

/* Read-only occupancy preflight for the candidate secure route (stream 2,
 * match 2, context 3; implemented in adreno8_smmu.c). The handover entry calls
 * it before staging anything; a false result rejects the handover without
 * writing registers or issuing SCM calls and leaves the uncertain ownership
 * flag alone, because nothing was transferred. A pass reports
 * HLOS-side register state only: the secure world's own reservations stay
 * invisible and translation behavior needs a device run. */
bool adreno8_secure_preflight(void);

/* Only one ownership transfer runs per boot. TERMINAL blocks further work;
 * mode 4 records a successful transfer as ASSIGNED, then advances to READY
 * after validating the secure translation. A failure closes READY without returning any pages. */
#define SECURE_XFER_NOT_STARTED 0u
#define SECURE_XFER_ATTEMPTED   1u
#define SECURE_XFER_TERMINAL    2u
#define SECURE_XFER_ASSIGNED    3u
#define SECURE_XFER_READY       4u

uint32_t adreno8_secure_xfer_state(void);

/* Before the first MP_ASSIGN, close the attempt without changing page ownership.
 * After ASSIGNED or READY, close the service while retaining its pages. Other states are
 * unchanged: this call cannot cancel an in-progress SMC. */
void adreno8_secure_xfer_abort(void);

/* Called only after mode 4 validates both the context and translate route. */
bool adreno8_secure_xfer_ready(void);

/* One isolated handover of the kernel's own kind-5 buffer: assign this space's
 * page-table pages, then the buffer's data pages, print the batch plan before
 * the first MP_ASSIGN, and stop. A true result means the CPU must never touch
 * those pages again; the pages stay retained whether the handover succeeded or
 * failed. Every later call returns false without issuing SCM calls. */
bool adreno8_secure_xfer(struct adreno8_gpu_space *space, const struct adreno8_memory *buffer);

#endif
