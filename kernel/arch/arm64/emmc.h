#ifndef EMMC_H
#define EMMC_H

#include <stdint.h>

/*
 * The SD card controller. This is how a Pi reads its own boot medium, so it is what gives
 * the port a filesystem -- everything tOS already knows how to do with a disk depends on
 * a block device existing first.
 */

int emmc_init(void);                                        /* 0 = a card was initialised */
int emmc_read(uint64_t lba, uint32_t count, void *buf);     /* 512-byte blocks */
int emmc_write(uint64_t lba, uint32_t count, const void *buf);
uint64_t emmc_blocks(void);                                 /* card size in blocks */

#endif
