#ifndef IMGLOAD_H
#define IMGLOAD_H

#include <stdint.h>

/* Pictures for the desktop: PNG (any kind) and baseline JPEG/JFIF, as 8-bit RGBA. */
int img_decode(const uint8_t *data, uint32_t len, uint8_t **rgba, uint32_t *w, uint32_t *h, const char **err);
int img_load(const char *path, uint8_t **rgba, uint32_t *w, uint32_t *h, const char **err);
uint8_t *img_scale(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h);   /* malloc'd */
int img_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);              /* 0 = ok */

#endif
