#ifndef ARM_FAT_H
#define ARM_FAT_H

#include <stdint.h>

/*
 * A read-only FAT32 reader for the card's boot partition -- the one the firmware already
 * uses, so a program can simply be copied onto the card next to kernel8.img and run.
 */

int fat_mount(void);                                      /* finds and mounts the first FAT partition */
int fat_read_file(const char *name, uint8_t *buf, uint32_t max);   /* bytes read, or -1 */
int fat_list(void (*cb)(const char *name, uint32_t size));

#endif
