/* Apps RSC DRV2 v3.8 ARC diagnostics and clock votes through ACTIVE TCS1. */
#ifndef _DEV_ADRENO8_RPMH_PRIVATE_H
#define _DEV_ADRENO8_RPMH_PRIVATE_H
#include "adreno8_private.h"

/* Returns true without MMIO if disabled. One attempt per kernel reads CX, then MX
 * after CX completes and TCS1 is restored. Failure stops subsequent requests;
 * every write requires a cold reset before kernel memory can be reused. */
bool adreno8_rpmh_arc_read(void);

/* gmuclk=1 requires explicit touchscreen/wfi/wfe_mode=0 and empty sleep/wake/fast
 * TCSes. Read this boot's CX/MX votes, retain max(inherited, level 128), confirm
 * each ACTIVE vote before clocks start, and keep it until cold reset. Once only.
 * gmuclk=2 prepares at stage 5 for a complete run ending at 5 or 17, from COLD
 * without a preceding ARC read. Validate both live CMD DB maps, then request
 * their highest legal selectors using WRITE
 * completion without resource READ. The same explicit parameters and TCS guards apply. */
bool adreno8_rpmh_clock_prepare(void);

#endif
