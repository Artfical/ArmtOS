#ifndef JPEG_DECODE_H
#define JPEG_DECODE_H

#include <stdint.h>

/* Baseline JPEG/JFIF to 8-bit RGB (malloc'd, w*h*3). 0 = ok; *err says why not (or what was
 * damaged, when part of the picture decoded). */
int jpeg_decode(const uint8_t *file, uint32_t len, uint8_t **out_rgb, uint32_t *out_w, uint32_t *out_h, const char **err);

#endif
