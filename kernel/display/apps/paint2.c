#include "../uiapp.h"
#include "../uihost.h"
#include "../imgload.h"
#include "string.h"
#include "memory.h"

/* Paint: brush, eraser, line, rectangle, ellipse and fill on an RGBA canvas; PNG in and out. */

#define CW_ 640
#define CH_ 400
#define TOOL_W 56
#define BAR_H 46

enum { T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_COUNT };
static const char *const tool_name[T_COUNT] = { "Brush", "Eraser", "Line", "Box", "Oval", "Fill" };

static const uint32_t palette[16] = {
    0x111111, 0xFFFFFF, 0x9CA3AF, 0x4B5563, 0xEF4444, 0xF97316, 0xF59E0B, 0xFDE047,
    0x22C55E, 0x14B8A6, 0x06B6D4, 0x3B82F6, 0x6366F1, 0xA855F7, 0xEC4899, 0x8B5E3C };
static const int sizes[4] = { 2, 5, 10, 18 };

typedef struct {
    uint8_t *px;                  /* CW_ x CH_ RGBA */
    uint8_t *undo;
    int tool, size, drawing;
    uint32_t color;
    int sx, sy, lx, ly;           /* stroke start, last point (canvas coords) */
    int ox, oy;                   /* canvas origin inside the window */
    char path[224];
    char msg[64];
    ui_filedlg_t dlg;
    int dlg_for;                  /* 1 open, 2 save */
} paint_t;

static void put(paint_t *p, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= CW_ || y >= CH_) return;
    uint8_t *q = p->px + (y * CW_ + x) * 4;
    q[0] = (uint8_t)(c >> 16); q[1] = (uint8_t)(c >> 8); q[2] = (uint8_t)c; q[3] = 255;
}

static void dot(paint_t *p, int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) put(p, cx + x, cy + y, c);
}

static void line(paint_t *p, int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        dot(p, x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void ellipse(paint_t *p, int x0, int y0, int x1, int y1, int r, uint32_t c)
{
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    int rx = (x1 > x0 ? x1 - x0 : x0 - x1) / 2, ry = (y1 > y0 ? y1 - y0 : y0 - y1) / 2;
    int px = cx + rx, py = cy;
    for (int deg = 2; deg <= 360; deg += 2) {
        int nx = cx + rx * gfx_cos(deg) / 4096, ny = cy + ry * gfx_sin(deg) / 4096;
        line(p, px, py, nx, ny, r, c);
        px = nx; py = ny;
    }
}

static void flood(paint_t *p, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= CW_ || y >= CH_) return;
    uint8_t *s = p->px + (y * CW_ + x) * 4;
    uint32_t target = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
    if (target == c) return;
    /* a scanline fill with an explicit stack, so a big area cannot overflow the kernel stack */
    int cap = 8192;
    int *st = malloc((size_t)cap * 2 * sizeof(int));
    if (!st) return;
    int top = 0;
    st[top++] = x; st[top++] = y;
#define AT(X, Y) ((((uint32_t)p->px[((Y) * CW_ + (X)) * 4] << 16) | ((uint32_t)p->px[((Y) * CW_ + (X)) * 4 + 1] << 8) | p->px[((Y) * CW_ + (X)) * 4 + 2]))
    while (top) {
        int yy = st[--top], xx = st[--top];
        while (xx > 0 && AT(xx - 1, yy) == target) xx--;
        int up = 0, down = 0;
        for (; xx < CW_ && AT(xx, yy) == target; xx++) {
            put(p, xx, yy, c);
            if (yy > 0) {
                int t = AT(xx, yy - 1) == target;
                if (t && !up && top < cap * 2 - 2) { st[top++] = xx; st[top++] = yy - 1; }
                up = t;
            }
            if (yy < CH_ - 1) {
                int t = AT(xx, yy + 1) == target;
                if (t && !down && top < cap * 2 - 2) { st[top++] = xx; st[top++] = yy + 1; }
                down = t;
            }
        }
    }
#undef AT
    free(st);
}

static void clear(paint_t *p) { for (int i = 0; i < CW_ * CH_; i++) { p->px[i * 4] = p->px[i * 4 + 1] = p->px[i * 4 + 2] = 255; p->px[i * 4 + 3] = 255; } }

static void open_png(uiapp_t *a, paint_t *p, const char *path)
{
    uint8_t *img; uint32_t w, h;
    const char *err;
    if (img_load(path, &img, &w, &h, &err) != 0) { strcpy(p->msg, "Could not open that picture"); return; }
    clear(p);
    uint8_t *s = (w == CW_ && h == CH_) ? img : img_scale(img, w, h, w > CW_ || h > CH_ ? CW_ : w, w > CW_ || h > CH_ ? CH_ : h);
    uint32_t dw = s == img ? w : (w > CW_ || h > CH_ ? CW_ : w), dh = s == img ? h : (w > CW_ || h > CH_ ? CH_ : h);
    if (s) for (uint32_t y = 0; y < dh && y < CH_; y++) memcpy(p->px + y * CW_ * 4, s + y * dw * 4, (dw < CW_ ? dw : CW_) * 4);
    if (s && s != img) free(s);
    free(img);
    strncpy(p->path, path, sizeof(p->path) - 1);
    strcpy(a->title, ui_basename(path));
}

static void p_init(uiapp_t *a, const char *arg)
{
    paint_t *p = a->st;
    p->px = malloc(CW_ * CH_ * 4);
    p->undo = malloc(CW_ * CH_ * 4);
    p->size = 1;
    p->color = palette[0];
    if (p->px) clear(p);
    if (arg && p->px) open_png(a, p, arg);
}

static void p_close(uiapp_t *a) { paint_t *p = a->st; if (p->px) free(p->px); if (p->undo) free(p->undo); }

static void p_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    paint_t *p = a->st;
    /* top bar: file buttons, sizes, colour swatches */
    gfx_fill(s, x, y, w, BAR_H, UI_TOOLBAR);
    gfx_hline(s, x, y + BAR_H - 1, w, 0, 120);
    ui_button(s, x + 8, y + 9, 54, 28, "New", UI_CARD2, UI_TEXT, 0);
    ui_button(s, x + 66, y + 9, 58, 28, "Open", UI_CARD2, UI_TEXT, 0);
    ui_button(s, x + 128, y + 9, 54, 28, "Save", UI_CARD2, UI_TEXT, 0);
    ui_button(s, x + 186, y + 9, 54, 28, "Undo", UI_CARD2, UI_TEXT, 0);
    for (int i = 0; i < 4; i++) {
        int bx = x + 252 + i * 30;
        gfx_rrect(s, bx, y + 9, 26, 28, 6, p->size == i ? UI_ACCENT : UI_CARD2, 255);
        gfx_circle(s, bx + 13, y + 23, sizes[i] / 2 + 1, UI_TEXT, 255);
    }
    for (int i = 0; i < 16; i++) {
        int bx = x + 380 + (i % 8) * 20, by = y + 5 + (i / 8) * 19;
        gfx_rrect(s, bx, by, 17, 17, 4, palette[i], 255);
        if (palette[i] == p->color) gfx_rrect_outline(s, bx - 2, by - 2, 21, 21, 6, UI_TEXT, 255);
    }
    gfx_rrect(s, x + 548, y + 9, 28, 28, 7, p->color, 255);
    gfx_rrect_outline(s, x + 548, y + 9, 28, 28, 7, 0xFFFFFF, 80);

    /* tools on the left */
    gfx_fill(s, x, y + BAR_H, TOOL_W, h - BAR_H, UI_SIDEBAR);
    for (int i = 0; i < T_COUNT; i++) {
        int ty = y + BAR_H + 10 + i * 46;
        gfx_rrect(s, x + 6, ty, TOOL_W - 12, 40, 8, p->tool == i ? UI_ACCENT : UI_CARD2, 255);
        gfx_text_center(s, &ui_font_small, x + 6, ty + 12, TOOL_W - 12, tool_name[i], UI_TEXT, 255);
    }

    /* the canvas, centred in what is left */
    int ax = x + TOOL_W, aw = w - TOOL_W, ay = y + BAR_H, ah = h - BAR_H - 22;
    gfx_fill(s, ax, ay, aw, ah + 22, UI_SIDEBAR);
    p->ox = TOOL_W + (aw - CW_) / 2; p->oy = BAR_H + (ah - CH_) / 2;
    if (p->ox < TOOL_W + 4) p->ox = TOOL_W + 4;
    if (p->oy < BAR_H + 4) p->oy = BAR_H + 4;
    gfx_shadow(s, x + p->ox, y + p->oy, CW_, CH_, 160);
    gfx_image_t im = { CW_, CH_, p->px };
    gfx_surface_t c = *s;
    gfx_clip(&c, ax, ay, aw, ah);
    if (p->px) gfx_blit(&c, &im, x + p->ox, y + p->oy, 255);
    /* the shape being dragged out, previewed before it is drawn in */
    if (p->drawing && (p->tool == T_LINE || p->tool == T_RECT || p->tool == T_ELLIPSE)) {
        int x0 = x + p->ox + p->sx, y0 = y + p->oy + p->sy, x1 = x + p->ox + p->lx, y1 = y + p->oy + p->ly;
        if (p->tool == T_LINE) gfx_capsule(&c, x0, y0, x1, y1, sizes[p->size] * 2, p->color, 200);
        else {
            int l = x0 < x1 ? x0 : x1, t = y0 < y1 ? y0 : y1, rw = x0 < x1 ? x1 - x0 : x0 - x1, rh = y0 < y1 ? y1 - y0 : y0 - y1;
            if (p->tool == T_RECT) gfx_rrect_outline(&c, l, t, rw + 1, rh + 1, 0, p->color, 220);
            else gfx_rrect_outline(&c, l, t, rw + 1, rh + 1, rw < rh ? rw / 2 : rh / 2, p->color, 220);
        }
    }
    gfx_text(s, &ui_font_small, ax + 10, y + h - 19, p->msg[0] ? p->msg : (p->path[0] ? p->path : "Untitled 640 x 400"), UI_DIM, 255);
    ui_filedlg_draw(&p->dlg, s, x, y, w, h);
}

static void dlg_result(uiapp_t *a, paint_t *p)
{
    if (p->dlg_for == 1) open_png(a, p, p->dlg.result);
    else {
        strncpy(p->path, p->dlg.result, sizeof(p->path) - 1);
        strcpy(p->msg, img_save_png(p->path, p->px, CW_, CH_) == 0 ? "Saved" : "Could not save");
        strcpy(a->title, ui_basename(p->path));
    }
}

static void snapshot(paint_t *p) { if (p->undo && p->px) memcpy(p->undo, p->px, CW_ * CH_ * 4); }

static void p_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    paint_t *p = a->st;
    a->dirty = 1;
    if (!p->px) return;
    if (p->dlg.active) {
        if (ui_filedlg_press(&p->dlg, px, py, a->w, a->h) == 1) dlg_result(a, p);
        return;
    }
    if (py < BAR_H) {
        if (ui_hit(px, py, 8, 9, 54, 28)) { snapshot(p); clear(p); p->path[0] = 0; strcpy(a->title, "Paint"); }
        else if (ui_hit(px, py, 66, 9, 58, 28)) { ui_filedlg_open(&p->dlg, 0, 0, 0); p->dlg_for = 1; }
        else if (ui_hit(px, py, 128, 9, 54, 28)) {
            if (p->path[0]) strcpy(p->msg, img_save_png(p->path, p->px, CW_, CH_) == 0 ? "Saved" : "Could not save");
            else { ui_filedlg_open(&p->dlg, 1, 0, "Drawing.png"); p->dlg_for = 2; }
        } else if (ui_hit(px, py, 186, 9, 54, 28) && p->undo) {
            uint8_t *t = p->px; p->px = p->undo; p->undo = t;
        }
        for (int i = 0; i < 4; i++) if (ui_hit(px, py, 252 + i * 30, 9, 26, 28)) p->size = i;
        for (int i = 0; i < 16; i++) if (ui_hit(px, py, 380 + (i % 8) * 20, 5 + (i / 8) * 19, 17, 17)) p->color = palette[i];
        return;
    }
    if (px < TOOL_W) {
        int i = (py - BAR_H - 10) / 46;
        if (i >= 0 && i < T_COUNT) p->tool = i;
        return;
    }
    int cx = px - p->ox, cy = py - p->oy;
    if (cx < 0 || cy < 0 || cx >= CW_ || cy >= CH_) return;
    snapshot(p);
    p->msg[0] = 0;
    if (p->tool == T_FILL) { flood(p, cx, cy, p->color); return; }
    p->drawing = 1;
    p->sx = p->lx = cx; p->sy = p->ly = cy;
    if (p->tool == T_BRUSH || p->tool == T_ERASER) dot(p, cx, cy, sizes[p->size] / 2, p->tool == T_ERASER ? 0xFFFFFF : p->color);
}

static void p_drag(uiapp_t *a, int px, int py)
{
    paint_t *p = a->st;
    if (!p->drawing) return;
    int cx = px - p->ox, cy = py - p->oy;
    if (p->tool == T_BRUSH || p->tool == T_ERASER)
        line(p, p->lx, p->ly, cx, cy, sizes[p->size] / 2, p->tool == T_ERASER ? 0xFFFFFF : p->color);
    p->lx = cx; p->ly = cy;
    a->dirty = 1;
}

static void p_release(uiapp_t *a, int px, int py)
{
    paint_t *p = a->st;
    if (!p->drawing) return;
    int cx = px - p->ox, cy = py - p->oy;
    int r = sizes[p->size] / 2;
    if (p->tool == T_LINE) line(p, p->sx, p->sy, cx, cy, r, p->color);
    else if (p->tool == T_RECT) {
        line(p, p->sx, p->sy, cx, p->sy, r, p->color); line(p, cx, p->sy, cx, cy, r, p->color);
        line(p, cx, cy, p->sx, cy, r, p->color); line(p, p->sx, cy, p->sx, p->sy, r, p->color);
    } else if (p->tool == T_ELLIPSE) ellipse(p, p->sx, p->sy, cx, cy, r, p->color);
    p->drawing = 0;
    a->dirty = 1;
}

static void p_key(uiapp_t *a, int k)
{
    paint_t *p = a->st;
    if (p->dlg.active) { if (ui_filedlg_key(&p->dlg, k) == 1) dlg_result(a, p); a->dirty = 1; return; }
    if (k >= '1' && k <= '6') p->tool = k - '1';
    else if (k == 0x1A && p->undo) { uint8_t *t = p->px; p->px = p->undo; p->undo = t; }   /* Ctrl+Z */
    a->dirty = 1;
}

const uiapp_class_t app_paint = {
    "paint", "Paint", "paint", 760, 500, 1, sizeof(paint_t),
    p_init, p_draw, p_press, p_drag, p_release, p_key, 0, 0, p_close,
};
