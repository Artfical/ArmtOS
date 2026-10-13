#ifndef ARM_FB_H
#define ARM_FB_H

#include <stdint.h>

/* The display. The firmware owns the hardware, so a framebuffer is asked for through the
 * mailbox and then written to directly -- there is no mode setting to do here. */

int  fb_init(uint32_t width, uint32_t height);
int  fb_ready(void);
uint32_t fb_width(void);
uint32_t fb_height(void);
void fb_clear(uint32_t rgb);
void fb_pixel(uint32_t x, uint32_t y, uint32_t rgb);
void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
/* An 8x8 font, enough to show that the display works and to put a message on a screen. */
void fb_char(uint32_t x, uint32_t y, char c, uint32_t rgb);
void fb_text(uint32_t x, uint32_t y, const char *s, uint32_t rgb);

#endif
