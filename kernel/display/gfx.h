#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "ui_assets.h"

/*
 * Pixel drawing for the graphical desktop: 32-bit 0x00RRGGBB surfaces, clipping, alpha,
 * anti-aliased rounded rectangles, soft shadows, proportional text and pictures.
 */

typedef struct {
    int w, h;
    uint32_t *px;
    int cx0, cy0, cx1, cy1;          /* clip rectangle, [cx0,cx1) x [cy0,cy1) */
} gfx_surface_t;

typedef struct {
    int w, h;
    const uint8_t *rgba;             /* straight (not premultiplied) alpha */
} gfx_image_t;

int  gfx_init(void);                 /* inflates the asset blob; 0 = ok */
void gfx_surface_init(gfx_surface_t *s, uint32_t *px, int w, int h);
void gfx_clip(gfx_surface_t *s, int x, int y, int w, int h);   /* intersect */
void gfx_unclip(gfx_surface_t *s);

static inline uint32_t gfx_rgb(int r, int g, int b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);               /* t: 0 = a, 255 = b */

void gfx_fill(gfx_surface_t *s, int x, int y, int w, int h, uint32_t c);
void gfx_fill_alpha(gfx_surface_t *s, int x, int y, int w, int h, uint32_t c, int alpha);
void gfx_gradient_v(gfx_surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_rrect(gfx_surface_t *s, int x, int y, int w, int h, int r, uint32_t c, int alpha);
void gfx_rrect_outline(gfx_surface_t *s, int x, int y, int w, int h, int r, uint32_t c, int alpha);
void gfx_circle(gfx_surface_t *s, int cx, int cy, int r, uint32_t c, int alpha);
void gfx_hline(gfx_surface_t *s, int x, int y, int w, uint32_t c, int alpha);
void gfx_shadow(gfx_surface_t *s, int x, int y, int w, int h, int strength);   /* behind a w x h box */

/* copies a rectangle of `src` (same size surfaces or not) to `dst` at the same coordinates */
void gfx_copy_rect(gfx_surface_t *dst, const gfx_surface_t *src, int x, int y, int w, int h);

const gfx_image_t *gfx_image(const char *name);
void gfx_blit(gfx_surface_t *s, const gfx_image_t *im, int x, int y, int alpha);
void gfx_blit_scaled(gfx_surface_t *s, const gfx_image_t *im, int x, int y, int w, int h, int alpha);
void gfx_blit_tinted(gfx_surface_t *s, const gfx_image_t *im, int x, int y, uint32_t c, int alpha);  /* uses only alpha */

/* the wallpaper, scaled to w x h (RGB in, 0x00RRGGBB out); `blurred` is a frosted copy */
int  gfx_wallpaper(uint32_t *out, int w, int h, uint32_t *blurred);
int  gfx_wallpaper_from(uint32_t *out, int w, int h, uint32_t *blurred, const uint8_t *src, int sw, int sh, int bpp);

/* text: UTF-8, returns the advance width */
int  gfx_text(gfx_surface_t *s, const ui_font_t *f, int x, int y, const char *str, uint32_t c, int alpha);
int  gfx_text_width(const ui_font_t *f, const char *str);
void gfx_text_center(gfx_surface_t *s, const ui_font_t *f, int x, int y, int w, const char *str, uint32_t c, int alpha);

/* integer trigonometry: sine and cosine of whole degrees, scaled by 4096 */
int  gfx_sin(int deg);
int  gfx_cos(int deg);
/* a thick anti-aliased line with round ends; r4 is the half-width in quarter pixels */
void gfx_capsule(gfx_surface_t *s, int x0, int y0, int x1, int y1, int r4, uint32_t c, int alpha);

/* one code page 437 terminal cell, 9 x 18 */
void gfx_cell(gfx_surface_t *s, int x, int y, uint8_t ch, uint32_t fg, uint32_t bg);

#endif
