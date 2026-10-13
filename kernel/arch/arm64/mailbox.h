#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdint.h>

/* The property mailbox: the only way to ask the GPU firmware anything -- how much memory
 * the ARM side was given, which board this is, and later for a framebuffer. */

int mailbox_board_revision(uint32_t *out);
int mailbox_arm_memory(uint32_t *bytes);
/* Requests a linear framebuffer; returns its address, geometry, and the pixel order the
 * firmware actually settled on (1 = RGB, 0 = BGR) -- it does not always honour the request,
 * and writing the wrong order silently swaps red and blue. 0 = granted. */
int mailbox_framebuffer(uint32_t width, uint32_t height,
                        uint64_t *fb_addr, uint32_t *pitch, uint32_t *fb_size,
                        uint32_t *pixel_order);

#endif
