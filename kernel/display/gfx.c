#include "gfx.h"
#include "png.h"
#include "memory.h"
#include "string.h"

static uint8_t *assets;            /* the inflated blob */

int gfx_init(void)
{
    if (assets) return 0;
    uint8_t *buf = malloc(ui_blob_raw_len + 16);
    if (!buf) return -1;
    uint32_t got = 0;
    if (inflate_raw_buffer(ui_blob, ui_blob_len, buf, ui_blob_raw_len, &got) != 0 || got != ui_blob_raw_len) {
        free(buf);
        return -1;
    }
    assets = buf;
    return 0;
}

void gfx_surface_init(gfx_surface_t *s, uint32_t *px, int w, int h)
{
    s->w = w; s->h = h; s->px = px;
    gfx_unclip(s);
}

void gfx_unclip(gfx_surface_t *s) { s->cx0 = 0; s->cy0 = 0; s->cx1 = s->w; s->cy1 = s->h; }

void gfx_clip(gfx_surface_t *s, int x, int y, int w, int h)
{
    if (x > s->cx0) s->cx0 = x;
    if (y > s->cy0) s->cy0 = y;
    if (x + w < s->cx1) s->cx1 = x + w;
    if (y + h < s->cy1) s->cy1 = y + h;
    if (s->cx1 < s->cx0) s->cx1 = s->cx0;
    if (s->cy1 < s->cy0) s->cy1 = s->cy0;
}

/* clips a rectangle in place; 0 if nothing is left */
static int clip_rect(const gfx_surface_t *s, int *x, int *y, int *w, int *h)
{
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < s->cx0) x0 = s->cx0;
    if (y0 < s->cy0) y0 = s->cy0;
    if (x1 > s->cx1) x1 = s->cx1;
    if (y1 > s->cy1) y1 = s->cy1;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int t)
{
    uint32_t it = (uint32_t)(255 - t), tt = (uint32_t)t;
    uint32_t rb = ((a & 0xFF00FF) * it + (b & 0xFF00FF) * tt) >> 8;
    uint32_t g  = ((a & 0x00FF00) * it + (b & 0x00FF00) * tt) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

static inline void blend(uint32_t *p, uint32_t c, int a)
{
    if (a <= 0) return;
    if (a >= 255) { *p = c; return; }
    *p = gfx_mix(*p, c, a);
}

void gfx_fill(gfx_surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = s->px + (y + j) * s->w + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void gfx_fill_alpha(gfx_surface_t *s, int x, int y, int w, int h, uint32_t c, int alpha)
{
    if (alpha >= 255) { gfx_fill(s, x, y, w, h, c); return; }
    if (alpha <= 0 || !clip_rect(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = s->px + (y + j) * s->w + x;
        for (int i = 0; i < w; i++) p[i] = gfx_mix(p[i], c, alpha);
    }
}

void gfx_hline(gfx_surface_t *s, int x, int y, int w, uint32_t c, int alpha) { gfx_fill_alpha(s, x, y, w, 1, c, alpha); }

void gfx_gradient_v(gfx_surface_t *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    if (h <= 0) return;
    for (int j = 0; j < h; j++) gfx_fill(s, x, y + j, w, 1, gfx_mix(top, bottom, h > 1 ? j * 255 / (h - 1) : 0));
}

/* How much of the pixel (px,py) in a corner of radius r centred at (cx,cy) is inside, 0..255.
 * 4x4 supersampling in fixed point; only corner pixels ever get here. */
static int corner_cov(int px, int py, int cx, int cy, int r)
{
    int inside = 0;
    int r2 = r * r * 64;                          /* (r*8)^2 */
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int dx = (px * 8 + sx * 2 + 1) - cx * 8;
            int dy = (py * 8 + sy * 2 + 1) - cy * 8;
            if (dx * dx + dy * dy <= r2) inside++;
        }
    return inside * 255 / 16;
}

void gfx_rrect(gfx_surface_t *s, int x, int y, int w, int h, int r, uint32_t c, int alpha)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_fill_alpha(s, x, y, w, h, c, alpha); return; }
    gfx_fill_alpha(s, x, y + r, w, h - 2 * r, c, alpha);
    gfx_fill_alpha(s, x + r, y, w - 2 * r, r, c, alpha);
    gfx_fill_alpha(s, x + r, y + h - r, w - 2 * r, r, c, alpha);
    /* the four corners */
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int cov = corner_cov(i, j, r, r, r);
            if (!cov) continue;
            int a = cov * alpha / 255;
            int pts[4][2] = { { x + i, y + j }, { x + w - 1 - i, y + j }, { x + i, y + h - 1 - j }, { x + w - 1 - i, y + h - 1 - j } };
            for (int k = 0; k < 4; k++) {
                int px = pts[k][0], py = pts[k][1];
                if (px < s->cx0 || px >= s->cx1 || py < s->cy0 || py >= s->cy1) continue;
                blend(&s->px[py * s->w + px], c, a);
            }
        }
}

void gfx_rrect_outline(gfx_surface_t *s, int x, int y, int w, int h, int r, uint32_t c, int alpha)
{
    if (w <= 2 || h <= 2) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_fill_alpha(s, x + r, y, w - 2 * r, 1, c, alpha);
    gfx_fill_alpha(s, x + r, y + h - 1, w - 2 * r, 1, c, alpha);
    gfx_fill_alpha(s, x, y + r, 1, h - 2 * r, c, alpha);
    gfx_fill_alpha(s, x + w - 1, y + r, 1, h - 2 * r, c, alpha);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            int outer = corner_cov(i, j, r, r, r);
            int inner = r > 1 ? corner_cov(i, j, r, r, r - 1) : 0;
            int cov = outer - inner;
            if (cov <= 0) continue;
            int a = cov * alpha / 255;
            int pts[4][2] = { { x + i, y + j }, { x + w - 1 - i, y + j }, { x + i, y + h - 1 - j }, { x + w - 1 - i, y + h - 1 - j } };
            for (int k = 0; k < 4; k++) {
                int px = pts[k][0], py = pts[k][1];
                if (px < s->cx0 || px >= s->cx1 || py < s->cy0 || py >= s->cy1) continue;
                blend(&s->px[py * s->w + px], c, a);
            }
        }
}

void gfx_circle(gfx_surface_t *s, int cx, int cy, int r, uint32_t c, int alpha)
{
    gfx_rrect(s, cx - r, cy - r, r * 2, r * 2, r, c, alpha);
}

void gfx_copy_rect(gfx_surface_t *dst, const gfx_surface_t *src, int x, int y, int w, int h)
{
    if (!clip_rect(dst, &x, &y, &w, &h)) return;
    if (x + w > src->w) w = src->w - x;
    if (y + h > src->h) h = src->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++)
        memcpy(dst->px + (y + j) * dst->w + x, src->px + (y + j) * src->w + x, (size_t)w * 4);
}

/* ---- images ---- */

static gfx_image_t images[160];
static int images_ready;

const gfx_image_t *gfx_image(const char *name)
{
    if (!assets) return 0;
    if (!images_ready) {
        for (int i = 0; i < ui_image_count && i < 160; i++) {
            images[i].w = ui_images[i].w;
            images[i].h = ui_images[i].h;
            images[i].rgba = assets + ui_images[i].off;
        }
        images_ready = 1;
    }
    for (int i = 0; i < ui_image_count && i < 160; i++)
        if (!strcmp(ui_images[i].name, name)) return &images[i];
    return 0;
}

void gfx_blit(gfx_surface_t *s, const gfx_image_t *im, int x, int y, int alpha)
{
    if (!im) return;
    int bx = x, by = y, w = im->w, h = im->h;
    if (!clip_rect(s, &bx, &by, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        const uint8_t *src = im->rgba + ((by - y + j) * im->w + (bx - x)) * 4;
        uint32_t *p = s->px + (by + j) * s->w + bx;
        for (int i = 0; i < w; i++, src += 4) {
            int a = src[3] * alpha / 255;
            if (a) blend(&p[i], gfx_rgb(src[0], src[1], src[2]), a);
        }
    }
}

void gfx_blit_tinted(gfx_surface_t *s, const gfx_image_t *im, int x, int y, uint32_t c, int alpha)
{
    if (!im) return;
    int bx = x, by = y, w = im->w, h = im->h;
    if (!clip_rect(s, &bx, &by, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        const uint8_t *src = im->rgba + ((by - y + j) * im->w + (bx - x)) * 4;
        uint32_t *p = s->px + (by + j) * s->w + bx;
        for (int i = 0; i < w; i++, src += 4) {
            int a = src[3] * alpha / 255;
            if (a) blend(&p[i], c, a);
        }
    }
}

void gfx_blit_scaled(gfx_surface_t *s, const gfx_image_t *im, int x, int y, int w, int h, int alpha)
{
    if (!im || w <= 0 || h <= 0) return;
    if (w == im->w && h == im->h) { gfx_blit(s, im, x, y, alpha); return; }
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < s->cy0 || py >= s->cy1) continue;
        /* bilinear: source position in 8.8 fixed point */
        int sy = (j * 256 * im->h) / h;
        int y0 = sy >> 8, fy = sy & 255;
        int y1 = y0 + 1 < im->h ? y0 + 1 : y0;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < s->cx0 || px >= s->cx1) continue;
            int sx = (i * 256 * im->w) / w;
            int x0 = sx >> 8, fx = sx & 255;
            int x1 = x0 + 1 < im->w ? x0 + 1 : x0;
            const uint8_t *a = im->rgba + (y0 * im->w + x0) * 4, *b = im->rgba + (y0 * im->w + x1) * 4;
            const uint8_t *c = im->rgba + (y1 * im->w + x0) * 4, *d = im->rgba + (y1 * im->w + x1) * 4;
            int ch[4];
            for (int k = 0; k < 4; k++) {
                int top = a[k] * (256 - fx) + b[k] * fx, bot = c[k] * (256 - fx) + d[k] * fx;
                ch[k] = (top * (256 - fy) + bot * fy) >> 16;
            }
            int al = ch[3] * alpha / 255;
            if (al) blend(&s->px[py * s->w + px], gfx_rgb(ch[0], ch[1], ch[2]), al);
        }
    }
}

/* a window's shadow from the nine-slice picture: corners as they are, edges stretched */
void gfx_shadow(gfx_surface_t *s, int x, int y, int w, int h, int strength)
{
    const gfx_image_t *im = gfx_image("shadow");
    if (!im) return;
    const int pad = 40, slice = 52;            /* matches the generator: 40 px margin around the box */
    int X = x - pad, Y = y - pad, W = w + 2 * pad, H = h + 2 * pad;
    if (W < 2 * slice || H < 2 * slice) return;
    for (int j = 0; j < H; j++) {
        int py = Y + j;
        if (py < s->cy0 || py >= s->cy1) continue;
        int sy = j < slice ? j : (j >= H - slice ? im->h - (H - j) : slice);
        for (int i = 0; i < W; i++) {
            int px = X + i;
            if (px < s->cx0 || px >= s->cx1) continue;
            /* inside the box itself nothing shows, so skip the work */
            if (i >= pad && i < W - pad && j >= pad && j < H - pad) { i = W - pad - 1; continue; }
            int sx = i < slice ? i : (i >= W - slice ? im->w - (W - i) : slice);
            int a = im->rgba[(sy * im->w + sx) * 4 + 3] * strength / 255;
            if (a) blend(&s->px[py * s->w + px], 0, a);
        }
    }
}

/* ---- wallpaper ---- */

/* Any picture as the wallpaper: scaled to cover the screen (cropping the longer side, never
 * stretching), then a frosted copy for the glass bars. `bpp` is 3 (RGB) or 4 (RGBA). */
int gfx_wallpaper_from(uint32_t *out, int w, int h, uint32_t *blurred, const uint8_t *src, int sw, int sh, int bpp)
{
    if (!src || sw <= 0 || sh <= 0) return -1;
    int scale_num, scale_den;
    if ((int64_t)w * sh > (int64_t)h * sw) { scale_num = sw; scale_den = w; }
    else { scale_num = sh; scale_den = h; }
    int offx = (sw - (int)((int64_t)w * scale_num / scale_den)) / 2, offy = (sh - (int)((int64_t)h * scale_num / scale_den)) / 2;
    for (int y = 0; y < h; y++) {
        int sy = offy + (int)((int64_t)y * scale_num / scale_den);
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < w; x++) {
            int sx = offx + (int)((int64_t)x * scale_num / scale_den);
            if (sx >= sw) sx = sw - 1;
            const uint8_t *p = src + ((size_t)sy * sw + sx) * bpp;
            out[y * w + x] = gfx_rgb(p[0], p[1], p[2]);
        }
    }
    if (!blurred) return 0;
    /* frosted glass: three box blurs of radius 12 approximate a gaussian, horizontal then vertical */
    memcpy(blurred, out, (size_t)w * h * 4);
    uint32_t *line = malloc((size_t)(w > h ? w : h) * 4);
    if (!line) return 0;
    const int R = 12;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) {
            uint32_t *row = blurred + y * w;
            int sr = 0, sg = 0, sb = 0;
            for (int i = -R; i <= R; i++) {
                uint32_t c = row[i < 0 ? 0 : (i >= w ? w - 1 : i)];
                sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
            }
            for (int x = 0; x < w; x++) {
                line[x] = gfx_rgb(sr / (2 * R + 1), sg / (2 * R + 1), sb / (2 * R + 1));
                uint32_t a = row[x - R < 0 ? 0 : x - R], b = row[x + R + 1 >= w ? w - 1 : x + R + 1];
                sr += (int)((b >> 16) & 255) - (int)((a >> 16) & 255);
                sg += (int)((b >> 8) & 255) - (int)((a >> 8) & 255);
                sb += (int)(b & 255) - (int)(a & 255);
            }
            memcpy(row, line, (size_t)w * 4);
        }
        for (int x = 0; x < w; x++) {
            int sr = 0, sg = 0, sb = 0;
            for (int i = -R; i <= R; i++) {
                uint32_t c = blurred[(i < 0 ? 0 : (i >= h ? h - 1 : i)) * w + x];
                sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
            }
            for (int y = 0; y < h; y++) {
                line[y] = gfx_rgb(sr / (2 * R + 1), sg / (2 * R + 1), sb / (2 * R + 1));
                uint32_t a = blurred[(y - R < 0 ? 0 : y - R) * w + x], b = blurred[(y + R + 1 >= h ? h - 1 : y + R + 1) * w + x];
                sr += (int)((b >> 16) & 255) - (int)((a >> 16) & 255);
                sg += (int)((b >> 8) & 255) - (int)((a >> 8) & 255);
                sb += (int)(b & 255) - (int)(a & 255);
            }
            for (int y = 0; y < h; y++) blurred[y * w + x] = line[y];
        }
    }
    free(line);
    return 0;
}

int gfx_wallpaper(uint32_t *out, int w, int h, uint32_t *blurred)
{
    int idx = -1;
    for (int i = 0; i < ui_image_count; i++) if (!strcmp(ui_images[i].name, "wallpaper")) idx = i;
    if (idx < 0 || !assets) return -1;
    return gfx_wallpaper_from(out, w, h, blurred, assets + ui_images[idx].off, ui_images[idx].w, ui_images[idx].h, 3);
}

/* ---- text ---- */

static const ui_glyph_t *find_glyph(const ui_font_t *f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*s++ & 0x3F); }
    else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2;
    }
    *ps = (const char *)s;
    return c;
}

int gfx_text_width(const ui_font_t *f, const char *str)
{
    int w = 0;
    while (*str) {
        const ui_glyph_t *g = find_glyph(f, utf8_next(&str));
        if (g) w += g->adv; else w += f->size / 3;
    }
    return w;
}

int gfx_text(gfx_surface_t *s, const ui_font_t *f, int x, int y, const char *str, uint32_t c, int alpha)
{
    int pen = x;
    while (*str) {
        const ui_glyph_t *g = find_glyph(f, utf8_next(&str));
        if (!g) { pen += f->size / 3; continue; }
        if (g->w && g->h && assets) {
            const uint8_t *m = assets + g->off;
            int gx = pen + g->x, gy = y + g->y;
            for (int j = 0; j < g->h; j++) {
                int py = gy + j;
                if (py < s->cy0 || py >= s->cy1) continue;
                uint32_t *row = s->px + py * s->w;
                for (int i = 0; i < g->w; i++) {
                    int px = gx + i;
                    if (px < s->cx0 || px >= s->cx1) continue;
                    int a = m[j * g->w + i];
                    if (a) blend(&row[px], c, a * alpha / 255);
                }
            }
        }
        pen += g->adv;
    }
    return pen - x;
}

void gfx_text_center(gfx_surface_t *s, const ui_font_t *f, int x, int y, int w, const char *str, uint32_t c, int alpha)
{
    gfx_text(s, f, x + (w - gfx_text_width(f, str)) / 2, y, str, c, alpha);
}

void gfx_cell(gfx_surface_t *s, int x, int y, uint8_t ch, uint32_t fg, uint32_t bg)
{
    int bx = x, by = y, w = UI_CELL_W, h = UI_CELL_H;
    if (!clip_rect(s, &bx, &by, &w, &h)) return;
    const uint8_t *m = assets ? assets + ui_mono_off + (uint32_t)ch * UI_CELL_W * UI_CELL_H : 0;
    for (int j = 0; j < h; j++) {
        uint32_t *row = s->px + (by + j) * s->w + bx;
        const uint8_t *mr = m ? m + (by - y + j) * UI_CELL_W + (bx - x) : 0;
        for (int i = 0; i < w; i++) {
            int a = mr ? mr[i] : 0;
            row[i] = a == 0 ? bg : (a == 255 ? fg : gfx_mix(bg, fg, a));
        }
    }
}

/* ---- lines and angles, in integers ---- */

static const int16_t sin_tab[360] = {
    0, 71, 143, 214, 286, 357, 428, 499, 570, 641, 711, 782, 852, 921, 991,
    1060, 1129, 1198, 1266, 1334, 1401, 1468, 1534, 1600, 1666, 1731, 1796, 1860, 1923, 1986,
    2048, 2110, 2171, 2231, 2290, 2349, 2408, 2465, 2522, 2578, 2633, 2687, 2741, 2793, 2845,
    2896, 2946, 2996, 3044, 3091, 3138, 3183, 3228, 3271, 3314, 3355, 3396, 3435, 3474, 3511,
    3547, 3582, 3617, 3650, 3681, 3712, 3742, 3770, 3798, 3824, 3849, 3873, 3896, 3917, 3937,
    3956, 3974, 3991, 4006, 4021, 4034, 4046, 4056, 4065, 4074, 4080, 4086, 4090, 4094, 4095,
    4096, 4095, 4094, 4090, 4086, 4080, 4074, 4065, 4056, 4046, 4034, 4021, 4006, 3991, 3974,
    3956, 3937, 3917, 3896, 3873, 3849, 3824, 3798, 3770, 3742, 3712, 3681, 3650, 3617, 3582,
    3547, 3511, 3474, 3435, 3396, 3355, 3314, 3271, 3228, 3183, 3138, 3091, 3044, 2996, 2946,
    2896, 2845, 2793, 2741, 2687, 2633, 2578, 2522, 2465, 2408, 2349, 2290, 2231, 2171, 2110,
    2048, 1986, 1923, 1860, 1796, 1731, 1666, 1600, 1534, 1468, 1401, 1334, 1266, 1198, 1129,
    1060, 991, 921, 852, 782, 711, 641, 570, 499, 428, 357, 286, 214, 143, 71,
    0, -71, -143, -214, -286, -357, -428, -499, -570, -641, -711, -782, -852, -921, -991,
    -1060, -1129, -1198, -1266, -1334, -1401, -1468, -1534, -1600, -1666, -1731, -1796, -1860, -1923, -1986,
    -2048, -2110, -2171, -2231, -2290, -2349, -2408, -2465, -2522, -2578, -2633, -2687, -2741, -2793, -2845,
    -2896, -2946, -2996, -3044, -3091, -3138, -3183, -3228, -3271, -3314, -3355, -3396, -3435, -3474, -3511,
    -3547, -3582, -3617, -3650, -3681, -3712, -3742, -3770, -3798, -3824, -3849, -3873, -3896, -3917, -3937,
    -3956, -3974, -3991, -4006, -4021, -4034, -4046, -4056, -4065, -4074, -4080, -4086, -4090, -4094, -4095,
    -4096, -4095, -4094, -4090, -4086, -4080, -4074, -4065, -4056, -4046, -4034, -4021, -4006, -3991, -3974,
    -3956, -3937, -3917, -3896, -3873, -3849, -3824, -3798, -3770, -3742, -3712, -3681, -3650, -3617, -3582,
    -3547, -3511, -3474, -3435, -3396, -3355, -3314, -3271, -3228, -3183, -3138, -3091, -3044, -2996, -2946,
    -2896, -2845, -2793, -2741, -2687, -2633, -2578, -2522, -2465, -2408, -2349, -2290, -2231, -2171, -2110,
    -2048, -1986, -1923, -1860, -1796, -1731, -1666, -1600, -1534, -1468, -1401, -1334, -1266, -1198, -1129,
    -1060, -991, -921, -852, -782, -711, -641, -570, -499, -428, -357, -286, -214, -143, -71
};

int gfx_sin(int deg) { deg %= 360; if (deg < 0) deg += 360; return sin_tab[deg]; }
int gfx_cos(int deg) { return gfx_sin(deg + 90); }

/* A segment with round ends and radius r (in 1/4 pixels), anti-aliased: each pixel near it is
 * covered by how far its centre lies from the segment. All in 1/16-pixel fixed point. */
void gfx_capsule(gfx_surface_t *s, int x0, int y0, int x1, int y1, int r4, uint32_t c, int alpha)
{
    int R = r4 * 4;                                   /* radius in 1/16 px */
    int ax = x0 * 16, ay = y0 * 16, bx = x1 * 16, by = y1 * 16;
    int minx = (x0 < x1 ? x0 : x1) - r4 / 4 - 2, maxx = (x0 > x1 ? x0 : x1) + r4 / 4 + 2;
    int miny = (y0 < y1 ? y0 : y1) - r4 / 4 - 2, maxy = (y0 > y1 ? y0 : y1) + r4 / 4 + 2;
    int dx = bx - ax, dy = by - ay;
    int64_t len2 = (int64_t)dx * dx + (int64_t)dy * dy;
    for (int py = miny; py <= maxy; py++) {
        if (py < s->cy0 || py >= s->cy1) continue;
        for (int px = minx; px <= maxx; px++) {
            if (px < s->cx0 || px >= s->cx1) continue;
            int qx = px * 16 + 8, qy = py * 16 + 8;
            int64_t t = len2 ? ((int64_t)(qx - ax) * dx + (int64_t)(qy - ay) * dy) * 1024 / len2 : 0;
            if (t < 0) t = 0;
            if (t > 1024) t = 1024;
            int cx = ax + (int)(dx * t / 1024), cy = ay + (int)(dy * t / 1024);
            int ex = qx - cx, ey = qy - cy;
            int d2 = ex * ex + ey * ey;
            /* distance to the edge, in 1/16 px: inside by more than 8 = fully covered */
            if (d2 >= (R + 8) * (R + 8)) continue;          /* clearly outside */
            int edge;
            if (R > 8 && d2 <= (R - 8) * (R - 8)) edge = 8;  /* clearly inside */
            else {
                int d = R > 8 ? R - 8 : 0;
                while ((d + 1) * (d + 1) <= d2) d++;         /* integer sqrt over the thin rim only */
                edge = R - d;
            }
            int cov = edge >= 8 ? 255 : edge <= -8 ? 0 : (edge + 8) * 255 / 16;
            if (cov) blend(&s->px[py * s->w + px], c, cov * alpha / 255);
        }
    }
}
