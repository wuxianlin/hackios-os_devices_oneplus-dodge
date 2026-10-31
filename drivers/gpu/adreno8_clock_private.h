/* Fixed Sun GMU/HUB clock path; RPMh votes remain held until cold reset. */
#ifndef _DEV_ADRENO8_CLOCK_PRIVATE_H
#define _DEV_ADRENO8_CLOCK_PRIVATE_H
#include "adreno8_private.h"

extern struct adreno8_block adreno8_gcc;

/* Before GMU firmware release: confirm resource votes, then GMU 650 / HUB 200 MHz.
 * After the START ACK: lower only the GPU PLL0 output to GMU 500 MHz.
 * gmuclk=2 accepts complete runs through stage 5 or 17. Only a stage-17 run
 * may lower the clock after its START ACK in stage 14.
 * Both return true without accesses for the default crystal-clock experiment.
 * Any failure closes this path and requires a cold reset; votes and clocks stay owned. */
bool adreno8_clock_boot(void);
bool adreno8_clock_started(void);

#endif
