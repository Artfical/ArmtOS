#ifndef UI_ASSETS_H
#define UI_ASSETS_H

#include <stdint.h>

/* The graphical desktop's fonts and pictures, generated into ui_assets.c by
 * tools/gen_ui_assets.py. All pixel data lives in one DEFLATE blob that gfx_init() inflates;
 * the offsets below index into the inflated bytes. */

typedef struct {
    uint32_t cp;            /* Unicode code point */
    int16_t  w, h;          /* bitmap size (8-bit coverage) */
    int16_t  x, y;          /* bitmap top-left relative to the pen at the top of the ascent */
    int16_t  adv;           /* how far the pen moves */
    uint32_t off;
} ui_glyph_t;

typedef struct {
    int size, ascent, descent, line_h;
    int count;
    const ui_glyph_t *glyphs;     /* sorted by code point */
} ui_font_t;

typedef struct {
    const char *name;
    int w, h;
    uint32_t off;                 /* RGBA, except "wallpaper" which is RGB */
} ui_image_t;

extern const uint8_t  ui_blob[];
extern const uint32_t ui_blob_len, ui_blob_raw_len;

extern const ui_font_t ui_font_regular, ui_font_medium, ui_font_small, ui_font_title, ui_font_huge;

/* the 9x18 terminal cell font, 256 cells in code page 437 order */
#define UI_CELL_W 9
#define UI_CELL_H 18
extern const uint32_t ui_mono_off;

extern const ui_image_t ui_images[];
extern const int ui_image_count;

#endif
