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

#endif
