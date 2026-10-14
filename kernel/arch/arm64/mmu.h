#ifndef ARM_MMU_H
#define ARM_MMU_H

#include <stdint.h>

/*
 * Paging. Until this runs, every access goes straight to memory with the caches off, which
 * is both slow and wrong for peripherals: without the MMU there is no way to say "this
 * range is a device, do not reorder or cache writes to it", and a driver that works only
 * because everything is uncached stops working the moment caches are turned on.
 */

void mmu_init(void);                     /* identity-maps RAM and the peripherals, then enables it */
int  mmu_enabled(void);
/* Opens a range to EL0. A .t program needs its own code and stack reachable from user mode;
 * everything else stays unreachable, which is what keeps a misbehaving program contained. */
void mmu_set_user(uint64_t addr, uint64_t size);
/* Makes a range non-cacheable (granularity is the 2 MB block); for memory the GPU reads. */
void mmu_set_uncached(uint64_t addr, uint64_t size);

#endif
