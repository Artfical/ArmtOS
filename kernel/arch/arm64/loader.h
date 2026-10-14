#ifndef ARM_LOADER_H
#define ARM_LOADER_H

#include <stdint.h>

/*
 * Running a `.t` program.
 *
 * The image is a flat binary that expects to live at a fixed address, with a 4-byte footer
 * saying how much memory it needs beyond what the file holds (its .bss). It is copied
 * there, the range is opened to EL0, and the program is started in user mode.
 */

#define USER_LOAD_ADDR  0x200000ull
#define USER_REGION     0x200000ull      /* one 2 MB block: code, data and stack */
#define USER_STACK_TOP  (USER_LOAD_ADDR + USER_REGION - 16)

int loader_run(const uint8_t *image, uint32_t len);   /* returns when the program exits */

#endif
