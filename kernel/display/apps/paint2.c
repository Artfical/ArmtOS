#include "../uiapp.h"
#include "../uihost.h"
#include "../imgload.h"
#include "string.h"
#include "memory.h"

/*
 * Paint, laid out like Windows 11's: File / Edit / View menus with undo and redo, a ribbon of
 * Selection, Image, Tools, Brushes, Shapes, Size and Colors, a Layers panel, the canvas with
 * zoom, and a status bar with the cursor, the selection, the canvas size and a zoom slider.
 * Everything works on RGBA layers; integer arithmetic only, like every app here.
 */

#define MAXL 6
#define UNDO 8
#define MENU_H 30
#define RIB_H 92
#define STAT_H 28
#define LAYER_W 132

enum { T_SELECT, T_PENCIL, T_FILL, T_TEXT, T_ERASER, T_PICKER, T_ZOOM,
       T_BRUSH, T_AIR, T_CALLI, T_MARKER,
       T_LINE, T_RECT, T_RRECT, T_ELLIPSE, T_TRI, T_DIAMOND, T_STAR, T_ARROW, T_HEART, T_COUNT };
enum { FILL_OUTLINE, FILL_SOLID, FILL_BOTH };

typedef struct { int w, h, n; uint8_t *px[MAXL]; } snap_t;

typedef struct {
    int W, H;                       /* canvas size */
    uint8_t *layer[MAXL];
    int visible[MAXL], nl, cur;
    uint8_t *comp;                  /* the layers merged, what is shown and saved */
    int comp_dirty;
    snap_t undo[UNDO], redo[UNDO];
    int nundo, nredo;
    int tool, shape_fill, size;
    uint32_t c1, c2;
    int slot;                       /* 0: palette clicks set colour 1, 1: colour 2 */
    int zoom;                       /* percent */
    int panx, pany;
    int drawing, sx, sy, lx, ly;    /* in canvas pixels */
    int curx, cury, in_canvas;
    /* selection: a rectangle, and while moving, a floating copy of it */
    int sel, selx, sely, selw, selh;
    uint8_t *floatp; int fx, fy, fw, fh, moving, mdx, mdy;
    uint8_t *clip; int cw, ch;
    /* text being typed */
    int typing, tx, ty;
    char text[96];
    /* chrome */
    int menu;                       /* 0 none, 1 File, 2 Edit, 3 View */
    int show_layers, show_grid, show_status;
    int color_dlg, hue, cs, cv;     /* the colour editor */
    char path[224];
    char msg[64];
    ui_filedlg_t dlg;
    int dlg_for;                    /* 1 open, 2 save */
    int lastw, lasth;               /* the window size the zoom was fitted to */
    uint32_t rng;
} pt_t;

/* ------------------------------------------------------------------ pixels */

static inline uint8_t *P(pt_t *p, int l, int x, int y) { return p->layer[l] + ((size_t)y * p->W + x) * 4; }

static void blendpx(uint8_t *q, uint32_t c, int a)
{
    if (a <= 0) return;
    if (a >= 255) { q[0] = (uint8_t)(c >> 16); q[1] = (uint8_t)(c >> 8); q[2] = (uint8_t)c; q[3] = 255; return; }
    int da = q[3], oa = a + da * (255 - a) / 255;
    if (!oa) return;
    q[0] = (uint8_t)((((c >> 16) & 255) * a + q[0] * da * (255 - a) / 255) / oa);
    q[1] = (uint8_t)((((c >> 8) & 255) * a + q[1] * da * (255 - a) / 255) / oa);
    q[2] = (uint8_t)(((c & 255) * a + q[2] * da * (255 - a) / 255) / oa);
    q[3] = (uint8_t)oa;
}

static void put(pt_t *p, int x, int y, uint32_t c, int a)
{
    if (x < 0 || y < 0 || x >= p->W || y >= p->H) return;
    if (p->sel && !p->moving && (x < p->selx || y < p->sely || x >= p->selx + p->selw || y >= p->sely + p->selh)) return;  /* paint stays inside a selection */
    blendpx(P(p, p->cur, x, y), c, a);
}

static void erase_px(pt_t *p, int x, int y)
{
    if (x < 0 || y < 0 || x >= p->W || y >= p->H) return;
    uint8_t *q = P(p, p->cur, x, y);
    if (p->cur == 0) { q[0] = (uint8_t)(p->c2 >> 16); q[1] = (uint8_t)(p->c2 >> 8); q[2] = (uint8_t)p->c2; q[3] = 255; }
    else q[0] = q[1] = q[2] = q[3] = 0;
}

static void disc(pt_t *p, int cx, int cy, int r, uint32_t c, int a, int erase)
{
    if (r < 1) { if (erase) erase_px(p, cx, cy); else put(p, cx, cy, c, a); return; }
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) { if (erase) erase_px(p, cx + x, cy + y); else put(p, cx + x, cy + y, c, a); }
}

static void line_with(pt_t *p, int x0, int y0, int x1, int y1, void (*stamp)(pt_t *, int, int))
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    for (;;) {
        stamp(p, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static const int sizes[6] = { 1, 3, 5, 8, 12, 20 };
static int rad(pt_t *p) { return sizes[p->size] / 2; }

static void st_pencil(pt_t *p, int x, int y) { disc(p, x, y, rad(p) / 2, p->c1, 255, 0); }
static void st_brush(pt_t *p, int x, int y) { disc(p, x, y, rad(p) + 1, p->c1, 255, 0); }
static void st_eraser(pt_t *p, int x, int y) { disc(p, x, y, rad(p) + 2, 0, 255, 1); }
static void st_marker(pt_t *p, int x, int y)
{
    int r = rad(p) + 2;
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) put(p, x + i, y + j, p->c1, 22);
}
static void st_calli(pt_t *p, int x, int y)
{
    int r = rad(p) + 3;
    for (int k = -r; k <= r; k++) { put(p, x + k, y - k, p->c1, 255); put(p, x + k + 1, y - k, p->c1, 255); }
}
static uint32_t rnd(pt_t *p) { p->rng = p->rng * 1103515245u + 12345u; return p->rng >> 16; }
static void st_air(pt_t *p, int x, int y)
{
    int r = rad(p) * 2 + 6;
    for (int k = 0; k < 14; k++) {
        int dx = (int)(rnd(p) % (uint32_t)(2 * r + 1)) - r, dy = (int)(rnd(p) % (uint32_t)(2 * r + 1)) - r;
        if (dx * dx + dy * dy <= r * r) put(p, x + dx, y + dy, p->c1, 255);
    }
}

/* ------------------------------------------------------------------ shapes */

/* a polygon filled by scanlines (even-odd), coordinates in canvas pixels */
static void poly_fill(pt_t *p, const int *xs, const int *ys, int n, uint32_t c)
{
    int miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) { if (ys[i] < miny) miny = ys[i]; if (ys[i] > maxy) maxy = ys[i]; }
    for (int y = miny; y <= maxy; y++) {
        int xi[32], k = 0;
        for (int i = 0; i < n && k < 32; i++) {
            int x0 = xs[i], y0 = ys[i], x1 = xs[(i + 1) % n], y1 = ys[(i + 1) % n];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) xi[k++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
        }
        for (int i = 1; i < k; i++) { int t = xi[i], j = i - 1; while (j >= 0 && xi[j] > t) { xi[j + 1] = xi[j]; j--; } xi[j + 1] = t; }
        for (int i = 0; i + 1 < k; i += 2) for (int x = xi[i]; x <= xi[i + 1]; x++) put(p, x, y, c, 255);
    }
}

static pt_t *stamp_target;
static void st_outline(pt_t *p, int x, int y) { disc(p, x, y, rad(p), p->c1, 255, 0); }

static void poly_line(pt_t *p, const int *xs, const int *ys, int n, int closed)
{
    for (int i = 0; i < n - 1 + closed; i++) line_with(p, xs[i], ys[i], xs[(i + 1) % n], ys[(i + 1) % n], st_outline);
}

static int shape_points(int tool, int x0, int y0, int x1, int y1, int *xs, int *ys)
{
    int l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0, t = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    int w = r - l, h = b - t, cx = (l + r) / 2, cy = (t + b) / 2, n = 0;
    switch (tool) {
    case T_RECT: xs[0] = l; ys[0] = t; xs[1] = r; ys[1] = t; xs[2] = r; ys[2] = b; xs[3] = l; ys[3] = b; return 4;
    case T_TRI: xs[0] = cx; ys[0] = t; xs[1] = r; ys[1] = b; xs[2] = l; ys[2] = b; return 3;
    case T_DIAMOND: xs[0] = cx; ys[0] = t; xs[1] = r; ys[1] = cy; xs[2] = cx; ys[2] = b; xs[3] = l; ys[3] = cy; return 4;
    case T_ARROW:
        xs[0] = l; ys[0] = t + h / 3; xs[1] = l + w * 3 / 5; ys[1] = t + h / 3; xs[2] = l + w * 3 / 5; ys[2] = t;
        xs[3] = r; ys[3] = cy; xs[4] = l + w * 3 / 5; ys[4] = b; xs[5] = l + w * 3 / 5; ys[5] = b - h / 3; xs[6] = l; ys[6] = b - h / 3;
        return 7;
    case T_STAR:
        for (int i = 0; i < 10; i++) {
            int rr = i % 2 ? 40 : 100, deg = i * 36;
            xs[n] = cx + gfx_sin(deg) * (w / 2) / 4096 * rr / 100;
            ys[n] = cy - gfx_cos(deg) * (h / 2) / 4096 * rr / 100;
            n++;
        }
        return n;
    case T_ELLIPSE: case T_RRECT: case T_HEART: default: {
        /* curves as polygons: an ellipse, a rounded rectangle, a heart */
        if (tool == T_ELLIPSE) {
            for (int d = 0; d < 360; d += 12) { xs[n] = cx + gfx_cos(d) * (w / 2) / 4096; ys[n] = cy + gfx_sin(d) * (h / 2) / 4096; n++; }
        } else if (tool == T_RRECT) {
            int rr = (w < h ? w : h) / 5;
            int ccx[4] = { r - rr, r - rr, l + rr, l + rr }, ccy[4] = { t + rr, b - rr, b - rr, t + rr }, start[4] = { 270, 0, 90, 180 };
            for (int c = 0; c < 4; c++)
                for (int d = 0; d <= 90; d += 15) { xs[n] = ccx[c] + gfx_cos(start[c] + d) * rr / 4096; ys[n] = ccy[c] + gfx_sin(start[c] + d) * rr / 4096; n++; }
        } else {
            for (int d = 0; d < 360; d += 12) {
                /* x = 16 sin^3, y = 13 cos - 5 cos2 - 2 cos3 - cos4, scaled into the box */
                int s = gfx_sin(d), c1 = gfx_cos(d), c2 = gfx_cos(2 * d), c3 = gfx_cos(3 * d), c4 = gfx_cos(4 * d);
                int hx = (int)((int64_t)16 * s * s / 4096 * s / 4096);                    /* *4096 */
                int hy = 13 * c1 - 5 * c2 - 2 * c3 - c4;                                    /* *4096, range about -17..12 */
                xs[n] = cx + hx * (w / 2) / (16 * 4096);
                ys[n] = cy - 1 * hy * (h / 2) / (15 * 4096) - h / 14;
                n++;
            }
        }
        return n;
    }
    }
}

static void draw_shape(pt_t *p, int tool, int x0, int y0, int x1, int y1)
{
    if (tool == T_LINE) { line_with(p, x0, y0, x1, y1, st_outline); return; }
    int xs[64], ys[64];
    int n = shape_points(tool, x0, y0, x1, y1, xs, ys);
    if (p->shape_fill != FILL_OUTLINE) poly_fill(p, xs, ys, n, p->shape_fill == FILL_SOLID ? p->c1 : p->c2);
    if (p->shape_fill != FILL_SOLID) poly_line(p, xs, ys, n, 1);
}

static void flood(pt_t *p, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= p->W || y >= p->H) return;
    uint8_t *s0 = P(p, p->cur, x, y);
    uint8_t tr = s0[0], tg = s0[1], tb = s0[2], ta = s0[3];
    if (ta == 255 && ((uint32_t)tr << 16 | (uint32_t)tg << 8 | tb) == c) return;
#define SAME(X, Y) ({ uint8_t *q_ = P(p, p->cur, X, Y); int d_ = (q_[0] > tr ? q_[0] - tr : tr - q_[0]) + (q_[1] > tg ? q_[1] - tg : tg - q_[1]) + \
                     (q_[2] > tb ? q_[2] - tb : tb - q_[2]) + (q_[3] > ta ? q_[3] - ta : ta - q_[3]); d_ <= 24 && !(q_[3] == 255 && q_[0] == (uint8_t)(c >> 16) && q_[1] == (uint8_t)(c >> 8) && q_[2] == (uint8_t)c); })
    int cap = 16384;
    int *st = malloc((size_t)cap * 2 * sizeof(int));
    if (!st) return;
    int top = 0;
    st[top++] = x; st[top++] = y;
    while (top) {
        int yy = st[--top], xx = st[--top];
        while (xx > 0 && SAME(xx - 1, yy)) xx--;
        int up = 0, down = 0;
        for (; xx < p->W && SAME(xx, yy); xx++) {
            uint8_t *q = P(p, p->cur, xx, yy);
            q[0] = (uint8_t)(c >> 16); q[1] = (uint8_t)(c >> 8); q[2] = (uint8_t)c; q[3] = 255;
            if (yy > 0) { int t = SAME(xx, yy - 1); if (t && !up && top < cap * 2 - 2) { st[top++] = xx; st[top++] = yy - 1; } up = t; }
            if (yy < p->H - 1) { int t = SAME(xx, yy + 1); if (t && !down && top < cap * 2 - 2) { st[top++] = xx; st[top++] = yy + 1; } down = t; }
        }
    }
#undef SAME
    free(st);
}

/* ------------------------------------------------------------------ layers, undo */

static uint8_t *blank(int w, int h, int white)
{
    uint8_t *b = malloc((size_t)w * h * 4);
    if (!b) return 0;
    if (white) memset(b, 255, (size_t)w * h * 4); else memset(b, 0, (size_t)w * h * 4);
    return b;
}

static void composite(pt_t *p)
{
    if (!p->comp_dirty) return;
    size_t n = (size_t)p->W * p->H;
    memset(p->comp, 0, n * 4);
    for (int l = 0; l < p->nl; l++) {
        if (!p->visible[l]) continue;
        uint8_t *s = p->layer[l];
        for (size_t i = 0; i < n; i++) if (s[i * 4 + 3]) blendpx(p->comp + i * 4, ((uint32_t)s[i * 4] << 16) | ((uint32_t)s[i * 4 + 1] << 8) | s[i * 4 + 2], s[i * 4 + 3]);
    }
    /* a floating selection being moved sits on top */
    if (p->floatp)
        for (int y = 0; y < p->fh; y++)
            for (int x = 0; x < p->fw; x++) {
                int cx = p->fx + x, cy = p->fy + y;
                if (cx < 0 || cy < 0 || cx >= p->W || cy >= p->H) continue;
                uint8_t *s = p->floatp + ((size_t)y * p->fw + x) * 4;
                if (s[3]) blendpx(p->comp + ((size_t)cy * p->W + cx) * 4, ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2], s[3]);
            }
    p->comp_dirty = 0;
}

static void snap_free(snap_t *s) { for (int i = 0; i < s->n; i++) if (s->px[i]) free(s->px[i]); s->n = 0; }

static int snap_take(pt_t *p, snap_t *s)
{
    s->w = p->W; s->h = p->H; s->n = 0;
    for (int l = 0; l < p->nl; l++) {
        s->px[l] = malloc((size_t)p->W * p->H * 4);
        if (!s->px[l]) { snap_free(s); return -1; }
        memcpy(s->px[l], p->layer[l], (size_t)p->W * p->H * 4);
        s->n++;
    }
    return 0;
}

static void snap_restore(pt_t *p, snap_t *s)
{
    for (int l = 0; l < p->nl; l++) free(p->layer[l]);
    p->W = s->w; p->H = s->h; p->nl = s->n;
    for (int l = 0; l < s->n; l++) { p->layer[l] = s->px[l]; s->px[l] = 0; }
    s->n = 0;
    if (p->cur >= p->nl) p->cur = p->nl - 1;
    free(p->comp);
    p->comp = malloc((size_t)p->W * p->H * 4);
    p->comp_dirty = 1;
}

static void checkpoint(pt_t *p)
{
    if (p->nundo == UNDO) { snap_free(&p->undo[0]); memmove(&p->undo[0], &p->undo[1], sizeof(snap_t) * (UNDO - 1)); p->nundo--; }
    if (snap_take(p, &p->undo[p->nundo]) == 0) p->nundo++;
    for (int i = 0; i < p->nredo; i++) snap_free(&p->redo[i]);
    p->nredo = 0;
}

static void undo(pt_t *p)
{
    if (!p->nundo) return;
    if (snap_take(p, &p->redo[p->nredo]) == 0) p->nredo++;
    snap_restore(p, &p->undo[--p->nundo]);
    p->sel = 0;
}

static void redo(pt_t *p)
{
    if (!p->nredo) return;
    if (snap_take(p, &p->undo[p->nundo]) == 0) p->nundo++;
    snap_restore(p, &p->redo[--p->nredo]);
    p->sel = 0;
}

static void new_canvas(pt_t *p, int w, int h)
{
    for (int l = 0; l < p->nl; l++) free(p->layer[l]);
    if (p->comp) free(p->comp);
    for (int i = 0; i < p->nundo; i++) snap_free(&p->undo[i]);
    for (int i = 0; i < p->nredo; i++) snap_free(&p->redo[i]);
    p->nundo = p->nredo = 0;
    p->W = w; p->H = h;
    p->layer[0] = blank(w, h, 1);
    p->visible[0] = 1;
    p->nl = 1; p->cur = 0;
    p->comp = malloc((size_t)w * h * 4);
    p->comp_dirty = 1;
    p->sel = 0; p->panx = p->pany = 0;
    if (p->floatp) { free(p->floatp); p->floatp = 0; }
}

/* geometry for the whole picture: every layer goes through `op` */
static void each_layer(pt_t *p, int nw, int nh, void (*op)(pt_t *, const uint8_t *, uint8_t *, int, int))
{
    checkpoint(p);
    for (int l = 0; l < p->nl; l++) {
        uint8_t *n = malloc((size_t)nw * nh * 4);
        if (!n) return;
        op(p, p->layer[l], n, nw, nh);
        free(p->layer[l]);
        p->layer[l] = n;
    }
    p->W = nw; p->H = nh;
    free(p->comp);
    p->comp = malloc((size_t)nw * nh * 4);
    p->comp_dirty = 1;
    p->sel = 0;
}

static void op_rot_r(pt_t *p, const uint8_t *s, uint8_t *d, int nw, int nh)
{ for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) memcpy(d + ((size_t)y * nw + x) * 4, s + ((size_t)(p->H - 1 - x) * p->W + y) * 4, 4); }
static void op_rot_l(pt_t *p, const uint8_t *s, uint8_t *d, int nw, int nh)
{ for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) memcpy(d + ((size_t)y * nw + x) * 4, s + ((size_t)x * p->W + (p->W - 1 - y)) * 4, 4); }
static void op_flip_h(pt_t *p, const uint8_t *s, uint8_t *d, int nw, int nh)
{ for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) memcpy(d + ((size_t)y * nw + x) * 4, s + ((size_t)y * p->W + (p->W - 1 - x)) * 4, 4); }
static void op_flip_v(pt_t *p, const uint8_t *s, uint8_t *d, int nw, int nh)
{ for (int y = 0; y < nh; y++) memcpy(d + (size_t)y * nw * 4, s + (size_t)(p->H - 1 - y) * p->W * 4, (size_t)nw * 4); }
static void op_crop(pt_t *p, const uint8_t *s, uint8_t *d, int nw, int nh)
{ for (int y = 0; y < nh; y++) memcpy(d + (size_t)y * nw * 4, s + ((size_t)(p->sely + y) * p->W + p->selx) * 4, (size_t)nw * 4); }
static void op_scale(pt_t *p, const uint8_t *s, uint8_t *d, int nw, int nh)
{
    uint8_t *t = img_scale(s, (uint32_t)p->W, (uint32_t)p->H, (uint32_t)nw, (uint32_t)nh);
    if (t) { memcpy(d, t, (size_t)nw * nh * 4); free(t); } else memset(d, 0, (size_t)nw * nh * 4);
}

static void add_layer(pt_t *p)
{
    if (p->nl >= MAXL) { strcpy(p->msg, "Six layers at most"); return; }
    checkpoint(p);
    uint8_t *b = blank(p->W, p->H, 0);
    if (!b) return;
    for (int l = p->nl; l > p->cur + 1; l--) { p->layer[l] = p->layer[l - 1]; p->visible[l] = p->visible[l - 1]; }
    p->layer[p->cur + 1] = b; p->visible[p->cur + 1] = 1;
    p->nl++; p->cur++;
    p->comp_dirty = 1;
}

static void del_layer(pt_t *p)
{
    if (p->nl <= 1) return;
    checkpoint(p);
    free(p->layer[p->cur]);
    for (int l = p->cur; l < p->nl - 1; l++) { p->layer[l] = p->layer[l + 1]; p->visible[l] = p->visible[l + 1]; }
    p->nl--;
    if (p->cur >= p->nl) p->cur = p->nl - 1;
    p->comp_dirty = 1;
}

static void move_layer(pt_t *p, int d)
{
    int o = p->cur + d;
    if (o < 0 || o >= p->nl) return;
    uint8_t *t = p->layer[o]; p->layer[o] = p->layer[p->cur]; p->layer[p->cur] = t;
    int v = p->visible[o]; p->visible[o] = p->visible[p->cur]; p->visible[p->cur] = v;
    p->cur = o;
    p->comp_dirty = 1;
}

/* ------------------------------------------------------------------ selection and clipboard */

static void lift_selection(pt_t *p)
{
    if (!p->sel || p->floatp) return;
    p->floatp = malloc((size_t)p->selw * p->selh * 4);
    if (!p->floatp) return;
    for (int y = 0; y < p->selh; y++) {
        memcpy(p->floatp + (size_t)y * p->selw * 4, P(p, p->cur, p->selx, p->sely + y), (size_t)p->selw * 4);
        for (int x = 0; x < p->selw; x++) {
            uint8_t *q = P(p, p->cur, p->selx + x, p->sely + y);
            if (p->cur == 0) { q[0] = (uint8_t)(p->c2 >> 16); q[1] = (uint8_t)(p->c2 >> 8); q[2] = (uint8_t)p->c2; q[3] = 255; }
            else q[0] = q[1] = q[2] = q[3] = 0;
        }
    }
    p->fx = p->selx; p->fy = p->sely; p->fw = p->selw; p->fh = p->selh;
}

static void drop_floating(pt_t *p)
{
    if (!p->floatp) return;
    for (int y = 0; y < p->fh; y++)
        for (int x = 0; x < p->fw; x++) {
            int cx = p->fx + x, cy = p->fy + y;
            if (cx < 0 || cy < 0 || cx >= p->W || cy >= p->H) continue;
            uint8_t *s = p->floatp + ((size_t)y * p->fw + x) * 4;
            if (s[3]) blendpx(P(p, p->cur, cx, cy), ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2], s[3]);
        }
    free(p->floatp);
    p->floatp = 0;
    p->selx = p->fx; p->sely = p->fy;
    p->comp_dirty = 1;
}

static void copy_sel(pt_t *p, int cut)
{
    if (!p->sel) { strcpy(p->msg, "Select something first"); return; }
    drop_floating(p);
    if (p->clip) free(p->clip);
    p->clip = malloc((size_t)p->selw * p->selh * 4);
    if (!p->clip) return;
    p->cw = p->selw; p->ch = p->selh;
    for (int y = 0; y < p->selh; y++) memcpy(p->clip + (size_t)y * p->cw * 4, P(p, p->cur, p->selx, p->sely + y), (size_t)p->cw * 4);
    if (cut) {
        checkpoint(p);
        for (int y = 0; y < p->selh; y++) for (int x = 0; x < p->selw; x++) erase_px(p, p->selx + x, p->sely + y);
        p->comp_dirty = 1;
    }
    strcpy(p->msg, cut ? "Cut" : "Copied");
}

static void paste(pt_t *p)
{
    if (!p->clip) { strcpy(p->msg, "Nothing to paste"); return; }
    drop_floating(p);
    checkpoint(p);
    p->floatp = malloc((size_t)p->cw * p->ch * 4);
    if (!p->floatp) return;
    memcpy(p->floatp, p->clip, (size_t)p->cw * p->ch * 4);
    p->fx = 0; p->fy = 0; p->fw = p->cw; p->fh = p->ch;
    p->sel = 1; p->selx = 0; p->sely = 0; p->selw = p->cw; p->selh = p->ch;
    p->tool = T_SELECT;
    p->comp_dirty = 1;
}

static void commit_text(pt_t *p)
{
    if (!p->typing) return;
    p->typing = 0;
    if (!p->text[0]) return;
    checkpoint(p);
    /* rendered into a scratch surface the size of the canvas, then blended into the layer */
    const ui_font_t *f = p->size < 2 ? &ui_font_small : p->size < 3 ? &ui_font_regular : p->size < 5 ? &ui_font_title : &ui_font_huge;
    int tw = gfx_text_width(f, p->text) + 4, th = f->line_h + 4;
    uint32_t *buf = malloc((size_t)tw * th * 4);
    if (!buf) return;
    for (int i = 0; i < tw * th; i++) buf[i] = 0x000000;
    gfx_surface_t s;
    gfx_surface_init(&s, buf, tw, th);
    gfx_text(&s, f, 2, 2, p->text, 0xFFFFFF, 255);              /* white on black: the brightness is the coverage */
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int a = (int)(buf[y * tw + x] & 255);
            if (a) put(p, p->tx + x, p->ty + y, p->c1, a);
        }
    free(buf);
    p->text[0] = 0;
    p->comp_dirty = 1;
}

/* ------------------------------------------------------------------ colours */

static const uint32_t palette[20] = {
    0x000000, 0x7F7F7F, 0x880015, 0xED1C24, 0xFF7F27, 0xFFF200, 0x22B14C, 0x00A2E8, 0x3F48CC, 0xA349A4,
    0xFFFFFF, 0xC3C3C3, 0xB97A57, 0xFFAEC9, 0xFFC90E, 0xEFE4B0, 0xB5E61D, 0x99D9EA, 0x7092BE, 0xC8BFE7 };

static uint32_t hsv(int h, int s, int v)       /* h 0..359, s v 0..255 */
{
    int region = h / 60, rem = (h % 60) * 255 / 60;
    int pp = v * (255 - s) / 255, q = v * (255 - s * rem / 255) / 255, t = v * (255 - s * (255 - rem) / 255) / 255;
    int r, g, b;
    switch (region) {
    case 0: r = v; g = t; b = pp; break;
    case 1: r = q; g = v; b = pp; break;
    case 2: r = pp; g = v; b = t; break;
    case 3: r = pp; g = q; b = v; break;
    case 4: r = t; g = pp; b = v; break;
    default: r = v; g = pp; b = q; break;
    }
    return gfx_rgb(r, g, b);
}

/* ------------------------------------------------------------------ the ribbon layout */

typedef struct { const char *glyph; int tool; } tb_t;
static const tb_t tools6[6] = { { "glyph_pencil", T_PENCIL }, { "glyph_paint-bucket", T_FILL }, { "glyph_type", T_TEXT },
                                { "glyph_eraser", T_ERASER }, { "glyph_pipette", T_PICKER }, { "glyph_zoom-in", T_ZOOM } };
static const tb_t brushes4[4] = { { "glyph_brush", T_BRUSH }, { "glyph_spray-can", T_AIR }, { "glyph_pen-tool", T_CALLI }, { "glyph_highlighter", T_MARKER } };
static const tb_t shapes9[9] = { { "glyph_minus", T_LINE }, { "glyph_square", T_RECT }, { "glyph_square-dashed", T_RRECT }, { "glyph_circle", T_ELLIPSE },
                                 { "glyph_triangle", T_TRI }, { "glyph_diamond", T_DIAMOND }, { "glyph_star", T_STAR }, { "glyph_arrow-right", T_ARROW },
                                 { "glyph_heart", T_HEART } };
static const char *const img_glyph[6] = { "glyph_crop", "glyph_scaling", "glyph_rotate-ccw", "glyph_rotate-cw", "glyph_flip-horizontal", "glyph_flip-vertical" };

/* group x positions */
#define G_SEL 8
#define G_IMG 96
#define G_TOOLS 206
#define G_BRUSH 316
#define G_SHAPE 396
#define G_FILLM 520
#define G_SIZE 600
#define G_COLOR 660
#define G_LAYERS (G_COLOR + 330)

static void ico(gfx_surface_t *s, int x, int y, const char *g, int on, int disabled)
{
    if (on) gfx_rrect(s, x - 5, y - 5, 26, 26, 5, ui_theme.dark ? 0x3A3C46 : 0xDCE6F8, 255);
    const gfx_image_t *im = gfx_image(g);
    if (im) gfx_blit_tinted(s, im, x, y, disabled ? UI_DIM : UI_TEXT, disabled ? 120 : 255);
}

static void group_label(gfx_surface_t *s, int x, int w, int y, const char *t)
{
    gfx_text_center(s, &ui_font_small, x, y + RIB_H - 22, w, t, UI_DIM, 255);
}

static void sep(gfx_surface_t *s, int x, int y) { gfx_fill(s, x, y + 12, 1, RIB_H - 24, UI_LINE); }

/* ------------------------------------------------------------------ geometry helpers */

static void view_rect(uiapp_t *a, pt_t *p, int *vx, int *vy, int *vw, int *vh)
{
    *vx = 0; *vy = MENU_H + RIB_H;
    *vw = a->w - (p->show_layers ? LAYER_W : 0);
    *vh = a->h - *vy - (p->show_status ? STAT_H : 0);
}

static void canvas_origin(uiapp_t *a, pt_t *p, int *ox, int *oy)
{
    int vx, vy, vw, vh;
    view_rect(a, p, &vx, &vy, &vw, &vh);
    int cw = p->W * p->zoom / 100, ch = p->H * p->zoom / 100;
    *ox = vx + (cw < vw - 40 ? (vw - cw) / 2 : 20) + p->panx;
    *oy = vy + (ch < vh - 40 ? (vh - ch) / 2 : 20) + p->pany;
}

static int to_canvas(uiapp_t *a, pt_t *p, int px, int py, int *cx, int *cy)
{
    int ox, oy;
    canvas_origin(a, p, &ox, &oy);
    int dx = px - ox, dy = py - oy;
    *cx = dx >= 0 ? dx * 100 / p->zoom : -1 - (-dx - 1) * 100 / p->zoom;
    *cy = dy >= 0 ? dy * 100 / p->zoom : -1 - (-dy - 1) * 100 / p->zoom;
    return *cx >= 0 && *cy >= 0 && *cx < p->W && *cy < p->H;
}

static void fit(uiapp_t *a, pt_t *p)
{
    int vx, vy, vw, vh;
    view_rect(a, p, &vx, &vy, &vw, &vh);
    int zx = (vw - 40) * 100 / p->W, zy = (vh - 40) * 100 / p->H;
    p->zoom = zx < zy ? zx : zy;
    if (p->zoom > 100) p->zoom = 100;
    if (p->zoom < 10) p->zoom = 10;
    p->panx = p->pany = 0;
}

static void set_zoom(pt_t *p, int z) { if (z < 10) z = 10; if (z > 800) z = 800; p->zoom = z; }

/* ------------------------------------------------------------------ drawing */

static void draw_menu_popup(gfx_surface_t *s, int x, int y, const char *const *items, int n, int w)
{
    int h = n * 26 + 8;
    gfx_shadow(s, x, y, w, h, 200);
    gfx_rrect(s, x, y, w, h, 8, ui_theme.menu, 250);
    gfx_rrect_outline(s, x, y, w, h, 8, ui_theme.dark ? 0xFFFFFF : 0x000000, 30);
    for (int i = 0; i < n; i++) {
        if (items[i][0] == '-') { gfx_hline(s, x + 10, y + 4 + i * 26 + 13, w - 20, UI_LINE, 255); continue; }
        gfx_text(s, &ui_font_regular, x + 14, y + 8 + i * 26, items[i], UI_TEXT, 255);
    }
}

static const char *const file_items[] = { "New", "Open...", "Save", "Save as...", "-", "Set as wallpaper" };
static const char *const edit_items[] = { "Undo", "Redo", "-", "Cut", "Copy", "Paste", "-", "Select all", "Delete selection", "Clear layer" };
static const char *const view_items[] = { "Zoom in", "Zoom out", "Actual size (100%)", "Fit to window", "-", "Gridlines", "Status bar", "Layers panel" };
#define NFILE 6
#define NEDIT 10
#define NVIEW 8

static void p_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    pt_t *p = a->st;
    if (!p->layer[0]) { gfx_text(s, &ui_font_regular, x + 20, y + 20, "Not enough memory for a canvas", UI_DIM, 255); return; }
    if (a->w != p->lastw || a->h != p->lasth) { p->lastw = a->w; p->lasth = a->h; fit(a, p); }
    strcpy(a->title, p->path[0] ? ui_basename(p->path) : "Untitled");
    strcat(a->title, " - Paint");

    /* menu bar with undo / redo */
    gfx_fill(s, x, y, w, MENU_H, UI_TOOLBAR);
    static const char *const mn[3] = { "File", "Edit", "View" };
    for (int i = 0; i < 3; i++) {
        if (p->menu == i + 1) gfx_rrect(s, x + 8 + i * 52, y + 3, 46, 24, 5, UI_CARD2, 255);
        gfx_text(s, &ui_font_regular, x + 16 + i * 52, y + 7, mn[i], UI_TEXT, 255);
    }
    ico(s, x + 180, y + 7, "glyph_undo-2", 0, !p->nundo);
    ico(s, x + 210, y + 7, "glyph_redo-2", 0, !p->nredo);

    /* the ribbon */
    int ry = y + MENU_H;
    gfx_fill(s, x, ry, w, RIB_H, ui_theme.dark ? 0x2A2B32 : 0xFAFAFC);
    gfx_hline(s, x, ry + RIB_H - 1, w, UI_LINE, 255);
    /* Selection */
    gfx_rrect(s, x + G_SEL + 6, ry + 10, 52, 44, 7, p->tool == T_SELECT ? (ui_theme.dark ? 0x3A3C46 : 0xDCE6F8) : 0, p->tool == T_SELECT ? 255 : 0);
    gfx_blit_tinted(s, gfx_image("glyph_square-dashed"), x + G_SEL + 24, ry + 16, UI_TEXT, 255);
    gfx_text_center(s, &ui_font_small, x + G_SEL + 6, ry + 36, 52, p->sel ? "Selected" : "Select", UI_TEXT, 255);
    group_label(s, x + G_SEL, 64, ry, "Selection");
    sep(s, x + G_IMG - 12, ry);
    /* Image */
    for (int i = 0; i < 6; i++) ico(s, x + G_IMG + (i % 3) * 32, ry + 14 + (i / 3) * 30, img_glyph[i], 0, i == 0 && !p->sel);
    group_label(s, x + G_IMG - 6, 100, ry, "Image");
    sep(s, x + G_TOOLS - 12, ry);
    /* Tools */
    for (int i = 0; i < 6; i++) ico(s, x + G_TOOLS + (i % 3) * 32, ry + 14 + (i / 3) * 30, tools6[i].glyph, p->tool == tools6[i].tool, 0);
    group_label(s, x + G_TOOLS - 6, 100, ry, "Tools");
    sep(s, x + G_BRUSH - 12, ry);
    /* Brushes */
    for (int i = 0; i < 4; i++) ico(s, x + G_BRUSH + (i % 2) * 32, ry + 14 + (i / 2) * 30, brushes4[i].glyph, p->tool == brushes4[i].tool, 0);
    group_label(s, x + G_BRUSH - 6, 70, ry, "Brushes");
    sep(s, x + G_SHAPE - 12, ry);
    /* Shapes */
    gfx_rrect(s, x + G_SHAPE - 4, ry + 8, 110, 58, 6, UI_FIELD, 255);
    for (int i = 0; i < 9; i++) {
        int ix = x + G_SHAPE + 2 + (i % 5) * 21, iy = ry + 16 + (i / 5) * 28;
        if (p->tool == shapes9[i].tool) gfx_rrect(s, ix - 3, iy - 4, 22, 24, 5, ui_theme.dark ? 0x3A3C46 : 0xDCE6F8, 255);
        ico(s, ix, iy, shapes9[i].glyph, 0, 0);
    }
    static const char *const fm[3] = { "Outline", "Filled", "Both" };
    for (int i = 0; i < 3; i++) {
        int by = ry + 8 + i * 20;
        if (p->shape_fill == i) gfx_rrect(s, x + G_FILLM - 4, by, 66, 18, 4, ui_theme.dark ? 0x3A3C46 : 0xDCE6F8, 255);
        gfx_text(s, &ui_font_small, x + G_FILLM + 2, by + 1, fm[i], UI_TEXT, 255);
    }
    group_label(s, x + G_SHAPE - 6, 194, ry, "Shapes");
    sep(s, x + G_SIZE - 12, ry);
    /* Size */
    for (int i = 0; i < 6; i++) {
        int by = ry + 10 + i * 10;
        if (p->size == i) gfx_rrect(s, x + G_SIZE - 4, by - 3, 44, 10, 3, UI_ACCENT, 70);
        gfx_fill(s, x + G_SIZE, by + 2 - sizes[i] / 6, 36, sizes[i] / 3 + 1, UI_TEXT);
    }
    group_label(s, x + G_SIZE - 10, 56, ry, "Size");
    sep(s, x + G_COLOR - 12, ry);
    /* Colors */
    gfx_circle(s, x + G_COLOR + 18, ry + 26, 16, p->c1, 255);
    gfx_rrect_outline(s, x + G_COLOR + 1, ry + 9, 34, 34, 17, p->slot == 0 ? UI_ACCENT : UI_LINE, 255);
    gfx_circle(s, x + G_COLOR + 18, ry + 60, 10, p->c2, 255);
    gfx_rrect_outline(s, x + G_COLOR + 7, ry + 49, 22, 22, 11, p->slot == 1 ? UI_ACCENT : UI_LINE, 255);
    for (int i = 0; i < 20; i++) {
        int cx = x + G_COLOR + 50 + (i % 10) * 22, cy = ry + 20 + (i / 10) * 24;
        gfx_circle(s, cx, cy, 9, palette[i], 255);
        gfx_rrect_outline(s, cx - 9, cy - 9, 18, 18, 9, 0x000000, 40);
    }
    /* edit colours: a rainbow wheel button */
    for (int d = 0; d < 360; d += 10) gfx_capsule(s, x + G_COLOR + 288, ry + 32, x + G_COLOR + 288 + gfx_cos(d) * 11 / 4096, ry + 32 + gfx_sin(d) * 11 / 4096, 6, hsv(d, 255, 255), 255);
    gfx_text(s, &ui_font_small, x + G_COLOR + 268, ry + 50, "Edit", UI_DIM, 255);
    group_label(s, x + G_COLOR, 310, ry, "Colors");
    sep(s, x + G_LAYERS - 12, ry);
    /* Layers toggle */
    gfx_rrect(s, x + G_LAYERS, ry + 10, 52, 44, 7, p->show_layers ? (ui_theme.dark ? 0x3A3C46 : 0xDCE6F8) : 0, p->show_layers ? 255 : 0);
    gfx_blit_tinted(s, gfx_image("glyph_layers"), x + G_LAYERS + 18, ry + 16, UI_TEXT, 255);
    group_label(s, x + G_LAYERS - 4, 60, ry, "Layers");

    /* the canvas */
    int vx, vy, vw, vh;
    view_rect(a, p, &vx, &vy, &vw, &vh);
    gfx_fill(s, x + vx, y + vy, vw, vh, ui_theme.dark ? 0x17181C : 0xE6E8EC);
    composite(p);
    int ox, oy;
    canvas_origin(a, p, &ox, &oy);
    int cw = p->W * p->zoom / 100, chh = p->H * p->zoom / 100;
    gfx_surface_t c = *s;
    gfx_clip(&c, x + vx, y + vy, vw, vh);
    gfx_shadow(&c, x + ox, y + oy, cw, chh, 120);
    /* the canvas pixels, scaled, over a checkerboard where transparent */
    int sx0 = x + ox, sy0 = y + oy;
    int vy0 = c.cy0 > sy0 ? c.cy0 : sy0, vy1 = c.cy1 < sy0 + chh ? c.cy1 : sy0 + chh;
    int vx0 = c.cx0 > sx0 ? c.cx0 : sx0, vx1 = c.cx1 < sx0 + cw ? c.cx1 : sx0 + cw;
    for (int py2 = vy0; py2 < vy1; py2++) {
        int cy2 = (py2 - sy0) * 100 / p->zoom;
        if (cy2 >= p->H) cy2 = p->H - 1;
        uint32_t *row = c.px + py2 * c.w;
        const uint8_t *src = p->comp + (size_t)cy2 * p->W * 4;
        for (int px2 = vx0; px2 < vx1; px2++) {
            int cx2 = (px2 - sx0) * 100 / p->zoom;
            if (cx2 >= p->W) cx2 = p->W - 1;
            const uint8_t *q = src + cx2 * 4;
            uint32_t bg = (((px2 - sx0) / 8 + (py2 - sy0) / 8) & 1) ? 0xFFFFFF : 0xE4E4E4;
            uint32_t col = gfx_rgb(q[0], q[1], q[2]);
            row[px2] = q[3] == 255 ? col : q[3] == 0 ? bg : gfx_mix(bg, col, q[3]);
        }
    }
    if (p->show_grid && p->zoom >= 400) {
        for (int gx = 0; gx <= p->W; gx++) gfx_fill_alpha(&c, sx0 + gx * p->zoom / 100, sy0, 1, chh, 0x808080, 90);
        for (int gy = 0; gy <= p->H; gy++) gfx_fill_alpha(&c, sx0, sy0 + gy * p->zoom / 100, cw, 1, 0x808080, 90);
    }
    /* the shape being dragged out, previewed */
    if (p->drawing && p->tool >= T_LINE) {
        int x0 = sx0 + p->sx * p->zoom / 100, y0 = sy0 + p->sy * p->zoom / 100, x1 = sx0 + p->lx * p->zoom / 100, y1 = sy0 + p->ly * p->zoom / 100;
        if (p->tool == T_LINE) gfx_capsule(&c, x0, y0, x1, y1, sizes[p->size] * 2 * p->zoom / 100 + 2, p->c1, 200);
        else {
            int xs[64], ys[64];
            int n = shape_points(p->tool, x0, y0, x1, y1, xs, ys);
            for (int i = 0; i < n; i++) gfx_capsule(&c, xs[i], ys[i], xs[(i + 1) % n], ys[(i + 1) % n], 3, p->c1, 220);
        }
    }
    /* the selection: a dashed rectangle */
    if (p->sel) {
        int bx = p->floatp ? p->fx : p->selx, by = p->floatp ? p->fy : p->sely;
        int rx = sx0 + bx * p->zoom / 100, ry2 = sy0 + by * p->zoom / 100, rw = p->selw * p->zoom / 100, rh = p->selh * p->zoom / 100;
        for (int k = 0; k < rw; k += 6) { gfx_fill(&c, rx + k, ry2, 3, 1, 0x000000); gfx_fill(&c, rx + k + 3, ry2, 3, 1, 0xFFFFFF);
                                          gfx_fill(&c, rx + k, ry2 + rh, 3, 1, 0x000000); gfx_fill(&c, rx + k + 3, ry2 + rh, 3, 1, 0xFFFFFF); }
        for (int k = 0; k < rh; k += 6) { gfx_fill(&c, rx, ry2 + k, 1, 3, 0x000000); gfx_fill(&c, rx, ry2 + k + 3, 1, 3, 0xFFFFFF);
                                          gfx_fill(&c, rx + rw, ry2 + k, 1, 3, 0x000000); gfx_fill(&c, rx + rw, ry2 + k + 3, 1, 3, 0xFFFFFF); }
    }
    /* text being typed */
    if (p->typing) {
        const ui_font_t *f = p->size < 2 ? &ui_font_small : p->size < 3 ? &ui_font_regular : p->size < 5 ? &ui_font_title : &ui_font_huge;
        int tx = sx0 + p->tx * p->zoom / 100, ty = sy0 + p->ty * p->zoom / 100;
        int tw = gfx_text(&c, f, tx + 2, ty + 2, p->text, p->c1, 255);
        gfx_rrect_outline(&c, tx - 2, ty - 2, tw + 12, f->line_h + 8, 3, UI_ACCENT, 255);
        if ((uihost_ms() / 500) % 2 == 0) gfx_fill(&c, tx + 3 + tw, ty + 3, 2, f->line_h, p->c1);
    }

    /* the layers panel */
    if (p->show_layers) {
        int lx = x + w - LAYER_W, ly = y + vy;
        gfx_fill(s, lx, ly, LAYER_W, vh, UI_SIDEBAR);
        gfx_fill(s, lx, ly, 1, vh, UI_LINE);
        ico(s, lx + 12, ly + 10, "glyph_plus", 0, p->nl >= MAXL);
        ico(s, lx + 40, ly + 10, "glyph_trash-2", 0, p->nl <= 1);
        ico(s, lx + 68, ly + 10, "glyph_arrow-up", 0, p->cur >= p->nl - 1);
        ico(s, lx + 96, ly + 10, "glyph_arrow-down", 0, p->cur <= 0);
        for (int i = p->nl - 1, k = 0; i >= 0; i--, k++) {
            int ty = ly + 40 + k * 76;
            if (i == p->cur) gfx_rrect(s, lx + 8, ty - 4, LAYER_W - 16, 72, 8, UI_ACCENT, 255);
            /* a thumbnail */
            int tw = 96, th = 56;
            gfx_fill(s, lx + 18, ty + 2, tw, th, 0xFFFFFF);
            for (int yy = 0; yy < th; yy++)
                for (int xx = 0; xx < tw; xx++) {
                    const uint8_t *q = p->layer[i] + ((size_t)(yy * p->H / th) * p->W + xx * p->W / tw) * 4;
                    if (q[3]) s->px[(ty + 2 + yy) * s->w + lx + 18 + xx] = gfx_mix(0xFFFFFF, gfx_rgb(q[0], q[1], q[2]), q[3]);
                }
            gfx_blit_tinted(s, gfx_image(p->visible[i] ? "glyph_eye" : "glyph_eye-off"), lx + 18, ty + 44, i == p->cur ? 0xFFFFFF : UI_TEXT, 255);
        }
    }

    /* the status bar */
    if (p->show_status) {
        int sy = y + h - STAT_H;
        gfx_fill(s, x, sy, w, STAT_H, UI_STATUS);
        gfx_hline(s, x, sy, w, UI_LINE, 255);
        char t[48] = "";
        if (p->in_canvas) { ui_num(t, p->curx); strcat(t, ", "); ui_num(t, p->cury); strcat(t, "px"); }
        gfx_blit_tinted(s, gfx_image("glyph_move"), x + 10, sy + 6, UI_DIM, 255);
        gfx_text(s, &ui_font_small, x + 32, sy + 7, t, UI_TEXT, 255);
        t[0] = 0;
        if (p->sel) { ui_num(t, p->selw); strcat(t, " \xC3\x97 "); ui_num(t, p->selh); strcat(t, "px"); }
        gfx_blit_tinted(s, gfx_image("glyph_square-dashed"), x + 140, sy + 6, UI_DIM, 255);
        gfx_text(s, &ui_font_small, x + 162, sy + 7, t, UI_TEXT, 255);
        t[0] = 0; ui_num(t, p->W); strcat(t, " \xC3\x97 "); ui_num(t, p->H); strcat(t, "px");
        gfx_blit_tinted(s, gfx_image("glyph_maximize"), x + 270, sy + 6, UI_DIM, 255);
        gfx_text(s, &ui_font_small, x + 292, sy + 7, t, UI_TEXT, 255);
        if (p->msg[0]) gfx_text(s, &ui_font_small, x + 420, sy + 7, p->msg, UI_ACCENT, 255);
        /* zoom: - slider + */
        int zx = x + w - 250;
        t[0] = 0; ui_num(t, p->zoom); strcat(t, "%");
        gfx_text(s, &ui_font_small, zx, sy + 7, t, UI_TEXT, 255);
        gfx_blit_tinted(s, gfx_image("glyph_zoom-out"), zx + 46, sy + 6, UI_TEXT, 255);
        gfx_rrect(s, zx + 70, sy + 12, 120, 4, 2, UI_LINE, 255);
        int knob = zx + 70 + (p->zoom - 10) * 120 / 790;
        gfx_rrect(s, zx + 70, sy + 12, knob - zx - 70, 4, 2, UI_ACCENT, 255);
        gfx_circle(s, knob, sy + 14, 6, UI_ACCENT, 255);
        gfx_blit_tinted(s, gfx_image("glyph_zoom-in"), zx + 198, sy + 6, UI_TEXT, 255);
    }

    /* popups */
    if (p->menu == 1) draw_menu_popup(s, x + 8, y + MENU_H, file_items, NFILE, 190);
    if (p->menu == 2) draw_menu_popup(s, x + 60, y + MENU_H, edit_items, NEDIT, 190);
    if (p->menu == 3) {
        draw_menu_popup(s, x + 112, y + MENU_H, view_items, NVIEW, 210);
        int on[3] = { p->show_grid, p->show_status, p->show_layers };
        for (int i = 0; i < 3; i++) if (on[i]) gfx_text(s, &ui_font_regular, x + 112 + 186, y + MENU_H + 8 + (5 + i) * 26, "\xE2\x9C\x93", UI_ACCENT, 255);
    }
    if (p->color_dlg) {
        int dx = x + w / 2 - 170, dy = y + 140;
        gfx_shadow(s, dx, dy, 340, 290, 220);
        gfx_rrect(s, dx, dy, 340, 290, 12, ui_theme.menu, 252);
        gfx_text(s, &ui_font_medium, dx + 16, dy + 12, "Edit colours", UI_TEXT, 255);
        for (int yy = 0; yy < 180; yy += 3)
            for (int xx = 0; xx < 220; xx += 3)
                gfx_fill(s, dx + 16 + xx, dy + 44 + yy, 3, 3, hsv(p->hue, xx * 255 / 219, 255 - yy * 255 / 179));
        for (int yy = 0; yy < 180; yy++) gfx_fill(s, dx + 250, dy + 44 + yy, 22, 1, hsv(yy * 359 / 179, 255, 255));
        gfx_rrect_outline(s, dx + 16 + p->cs * 219 / 255 - 5, dy + 44 + (255 - p->cv) * 179 / 255 - 5, 10, 10, 5, 0xFFFFFF, 255);
        gfx_fill(s, dx + 246, dy + 44 + p->hue * 179 / 359, 30, 2, 0xFFFFFF);
        uint32_t cur = hsv(p->hue, p->cs, p->cv);
        gfx_rrect(s, dx + 282, dy + 44, 42, 42, 8, cur, 255);
        ui_button(s, dx + 140, dy + 244, 86, 30, "Cancel", UI_CARD2, UI_TEXT, 0);
        ui_button(s, dx + 236, dy + 244, 88, 30, "OK", UI_ACCENT, 0xFFFFFF, 0);
    }
    ui_filedlg_draw(&p->dlg, s, x, y, w, h);
}

/* ------------------------------------------------------------------ actions */

static void save(uiapp_t *a, pt_t *p, int as)
{
    if (as || !p->path[0]) { ui_filedlg_open(&p->dlg, 1, 0, "Drawing.png"); p->dlg_for = 2; return; }
    drop_floating(p);
    composite(p);
    strcpy(p->msg, img_save_png(p->path, p->comp, (uint32_t)p->W, (uint32_t)p->H) == 0 ? "Saved" : "Could not save");
    (void)a;
}

static void open_file(uiapp_t *a, pt_t *p, const char *path)
{
    uint8_t *img; uint32_t w, h;
    const char *err;
    if (img_load(path, &img, &w, &h, &err) != 0) { strcpy(p->msg, "Could not open that picture"); return; }
    if (w > 2000 || h > 2000) {                  /* very large pictures come in scaled down */
        uint32_t nw = w, nh = h;
        while (nw > 2000 || nh > 2000) { nw /= 2; nh /= 2; }
        uint8_t *t = img_scale(img, w, h, nw, nh);
        free(img);
        if (!t) return;
        img = t; w = nw; h = nh;
    }
    new_canvas(p, (int)w, (int)h);
    if (p->layer[0]) memcpy(p->layer[0], img, (size_t)w * h * 4);
    free(img);
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->comp_dirty = 1;
    fit(a, p);
}

static void dlg_result(uiapp_t *a, pt_t *p)
{
    if (p->dlg_for == 1) open_file(a, p, p->dlg.result);
    else { strncpy(p->path, p->dlg.result, sizeof(p->path) - 1); save(a, p, 0); }
    p->dlg_for = 0;
}

static void menu_action(uiapp_t *a, pt_t *p, int menu, int i)
{
    drop_floating(p);
    if (menu == 1) {
        if (i == 0) { new_canvas(p, 800, 500); p->path[0] = 0; fit(a, p); }
        else if (i == 1) { ui_filedlg_open(&p->dlg, 0, 0, 0); p->dlg_for = 1; }
        else if (i == 2) save(a, p, 0);
        else if (i == 3) save(a, p, 1);
        else if (i == 5) {
            if (!p->path[0]) { strcpy(p->msg, "Save the picture first"); return; }
            save(a, p, 0);
            strcpy(p->msg, uihost_set_wallpaper(p->path) == 0 ? "Wallpaper set" : "Could not set the wallpaper");
        }
    } else if (menu == 2) {
        switch (i) {
        case 0: undo(p); break;
        case 1: redo(p); break;
        case 3: copy_sel(p, 1); break;
        case 4: copy_sel(p, 0); break;
        case 5: paste(p); break;
        case 7: p->sel = 1; p->selx = 0; p->sely = 0; p->selw = p->W; p->selh = p->H; p->tool = T_SELECT; break;
        case 8: if (p->sel) { checkpoint(p); for (int yy = 0; yy < p->selh; yy++) for (int xx = 0; xx < p->selw; xx++) erase_px(p, p->selx + xx, p->sely + yy); } break;
        case 9: checkpoint(p); p->sel = 0; for (int yy = 0; yy < p->H; yy++) for (int xx = 0; xx < p->W; xx++) erase_px(p, xx, yy); break;
        }
    } else if (menu == 3) {
        switch (i) {
        case 0: set_zoom(p, p->zoom * 3 / 2); break;
        case 1: set_zoom(p, p->zoom * 2 / 3); break;
        case 2: p->zoom = 100; p->panx = p->pany = 0; break;
        case 3: fit(a, p); break;
        case 5: p->show_grid = !p->show_grid; break;
        case 6: p->show_status = !p->show_status; break;
        case 7: p->show_layers = !p->show_layers; break;
        }
    }
    p->comp_dirty = 1;
}

static void image_action(uiapp_t *a, pt_t *p, int i)
{
    drop_floating(p);
    switch (i) {
    case 0: if (p->sel && p->selw > 0 && p->selh > 0) each_layer(p, p->selw, p->selh, op_crop); break;
    case 1:                                                   /* resize: alternate between half and double */
        if (p->W > 1200 || p->H > 1200) each_layer(p, p->W / 2, p->H / 2, op_scale);
        else each_layer(p, p->W * 2, p->H * 2, op_scale);
        break;
    case 2: each_layer(p, p->H, p->W, op_rot_l); break;
    case 3: each_layer(p, p->H, p->W, op_rot_r); break;
    case 4: each_layer(p, p->W, p->H, op_flip_h); break;
    case 5: each_layer(p, p->W, p->H, op_flip_v); break;
    }
    fit(a, p);
}

static int menu_hit(pt_t *p, int px, int py, int *item)
{
    int mx0 = p->menu == 1 ? 8 : p->menu == 2 ? 60 : 112, n = p->menu == 1 ? NFILE : p->menu == 2 ? NEDIT : NVIEW;
    int w = p->menu == 3 ? 210 : 190;
    if (px < mx0 || px >= mx0 + w || py < MENU_H + 4 || py >= MENU_H + 4 + n * 26) return 0;
    *item = (py - MENU_H - 4) / 26;
    return 1;
}

static void p_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    pt_t *p = a->st;
    a->dirty = 1;
    p->msg[0] = 0;
    if (!p->layer[0]) return;
    if (p->dlg.active) { if (ui_filedlg_press(&p->dlg, px, py, a->w, a->h) == 1) dlg_result(a, p); return; }
    if (p->color_dlg) {
        int dx = a->w / 2 - 170, dy = 140;
        if (ui_hit(px, py, dx + 16, dy + 44, 220, 180)) { p->cs = (px - dx - 16) * 255 / 219; p->cv = 255 - (py - dy - 44) * 255 / 179; }
        else if (ui_hit(px, py, dx + 246, dy + 44, 30, 180)) p->hue = (py - dy - 44) * 359 / 179;
        else if (ui_hit(px, py, dx + 140, dy + 244, 86, 30)) p->color_dlg = 0;
        else if (ui_hit(px, py, dx + 236, dy + 244, 88, 30)) {
            uint32_t c = hsv(p->hue, p->cs, p->cv);
            if (p->slot) p->c2 = c; else p->c1 = c;
            p->color_dlg = 0;
        }
        return;
    }
    if (p->menu) {
        int item;
        int m = p->menu;
        p->menu = 0;
        if (menu_hit(p, px, py, &item)) {
            const char *const *items = m == 1 ? file_items : m == 2 ? edit_items : view_items;
            if (items[item][0] != '-') menu_action(a, p, m, item);
            return;
        }
        if (py < MENU_H && px < 168) { int nm = (px - 8) / 52 + 1; if (nm != m && nm >= 1 && nm <= 3) p->menu = nm; return; }
    }
    if (py < MENU_H) {
        if (px >= 8 && px < 164) p->menu = (px - 8) / 52 + 1;
        else if (ui_hit(px, py, 174, 2, 26, 26)) { drop_floating(p); undo(p); }
        else if (ui_hit(px, py, 204, 2, 26, 26)) { drop_floating(p); redo(p); }
        return;
    }
    if (py < MENU_H + RIB_H) {
        int ry = py - MENU_H;
        if (ui_hit(px, ry, G_SEL + 6, 10, 52, 44)) { commit_text(p); p->tool = T_SELECT; }
        for (int i = 0; i < 6; i++) if (ui_hit(px, ry, G_IMG + (i % 3) * 32 - 5, 9 + (i / 3) * 30, 26, 26)) image_action(a, p, i);
        for (int i = 0; i < 6; i++) if (ui_hit(px, ry, G_TOOLS + (i % 3) * 32 - 5, 9 + (i / 3) * 30, 26, 26)) { commit_text(p); drop_floating(p); p->sel = 0; p->tool = tools6[i].tool; }
        for (int i = 0; i < 4; i++) if (ui_hit(px, ry, G_BRUSH + (i % 2) * 32 - 5, 9 + (i / 2) * 30, 26, 26)) { commit_text(p); p->tool = brushes4[i].tool; }
        for (int i = 0; i < 9; i++) if (ui_hit(px, ry, G_SHAPE + 2 + (i % 5) * 21 - 3, 11 + (i / 5) * 28, 21, 26)) { commit_text(p); p->tool = shapes9[i].tool; }
        for (int i = 0; i < 3; i++) if (ui_hit(px, ry, G_FILLM - 4, 8 + i * 20, 66, 18)) p->shape_fill = i;
        for (int i = 0; i < 6; i++) if (ui_hit(px, ry, G_SIZE - 4, 7 + i * 10, 44, 10)) p->size = i;
        if (ui_hit(px, ry, G_COLOR, 8, 36, 36)) p->slot = 0;
        if (ui_hit(px, ry, G_COLOR + 6, 48, 24, 24)) p->slot = 1;
        for (int i = 0; i < 20; i++) {
            int cx = G_COLOR + 50 + (i % 10) * 22, cy = 20 + (i / 10) * 24;
            if ((px - cx) * (px - cx) + (ry - cy) * (ry - cy) <= 100) { if (p->slot) p->c2 = palette[i]; else p->c1 = palette[i]; }
        }
        if (ui_hit(px, ry, G_COLOR + 272, 16, 32, 50)) { p->color_dlg = 1; p->hue = 210; p->cs = 200; p->cv = 230; }
        if (ui_hit(px, ry, G_LAYERS, 10, 52, 44)) p->show_layers = !p->show_layers;
        return;
    }
    /* the layers panel */
    if (p->show_layers && px >= a->w - LAYER_W) {
        int lx = a->w - LAYER_W, ly = MENU_H + RIB_H;
        if (ui_hit(px, py, lx + 7, ly + 5, 26, 26)) add_layer(p);
        else if (ui_hit(px, py, lx + 35, ly + 5, 26, 26)) del_layer(p);
        else if (ui_hit(px, py, lx + 63, ly + 5, 26, 26)) move_layer(p, 1);
        else if (ui_hit(px, py, lx + 91, ly + 5, 26, 26)) move_layer(p, -1);
        else {
            int k = (py - ly - 36) / 76, i = p->nl - 1 - k;
            if (py >= ly + 36 && i >= 0 && i < p->nl) {
                if (ui_hit(px, py, lx + 14, ly + 40 + k * 76 + 40, 24, 24)) { p->visible[i] = !p->visible[i]; p->comp_dirty = 1; }
                else { drop_floating(p); p->cur = i; }
            }
        }
        return;
    }
    /* the status bar's zoom slider */
    if (p->show_status && py >= a->h - STAT_H) {
        int zx = a->w - 250;
        if (ui_hit(px, py, zx + 42, 0 + a->h - STAT_H, 24, STAT_H)) set_zoom(p, p->zoom * 2 / 3);
        else if (ui_hit(px, py, zx + 194, a->h - STAT_H, 24, STAT_H)) set_zoom(p, p->zoom * 3 / 2);
        else if (px >= zx + 70 && px < zx + 190) set_zoom(p, 10 + (px - zx - 70) * 790 / 120);
        return;
    }
    /* on the canvas */
    int cx, cy;
    int inside = to_canvas(a, p, px, py, &cx, &cy);
    if (p->typing && p->tool == T_TEXT) commit_text(p);
    switch (p->tool) {
    case T_SELECT:
        if (p->sel && cx >= (p->floatp ? p->fx : p->selx) && cy >= (p->floatp ? p->fy : p->sely) &&
            cx < (p->floatp ? p->fx : p->selx) + p->selw && cy < (p->floatp ? p->fy : p->sely) + p->selh) {
            if (!p->floatp) { checkpoint(p); lift_selection(p); }
            p->moving = 1; p->mdx = cx - p->fx; p->mdy = cy - p->fy;
        } else {
            drop_floating(p);
            p->sel = 0; p->drawing = 1; p->sx = p->lx = cx; p->sy = p->ly = cy;
        }
        break;
    case T_PICKER:
        if (inside) {
            composite(p);
            const uint8_t *q = p->comp + ((size_t)cy * p->W + cx) * 4;
            uint32_t c = gfx_rgb(q[0], q[1], q[2]);
            if (p->slot) p->c2 = c; else p->c1 = c;
        }
        break;
    case T_ZOOM: set_zoom(p, p->zoom * 2); break;
    case T_FILL: if (inside) { checkpoint(p); flood(p, cx, cy, p->slot ? p->c2 : p->c1); p->comp_dirty = 1; } break;
    case T_TEXT: if (inside) { p->typing = 1; p->tx = cx; p->ty = cy; p->text[0] = 0; } break;
    default:
        if (!inside && p->tool < T_LINE) break;
        checkpoint(p);
        p->drawing = 1; p->sx = p->lx = cx; p->sy = p->ly = cy;
        if (p->tool == T_PENCIL) st_pencil(p, cx, cy);
        else if (p->tool == T_BRUSH) st_brush(p, cx, cy);
        else if (p->tool == T_ERASER) st_eraser(p, cx, cy);
        else if (p->tool == T_AIR) st_air(p, cx, cy);
        else if (p->tool == T_CALLI) st_calli(p, cx, cy);
        else if (p->tool == T_MARKER) st_marker(p, cx, cy);
        p->comp_dirty = 1;
        break;
    }
}

static void p_drag(uiapp_t *a, int px, int py)
{
    pt_t *p = a->st;
    int cx, cy;
    p->in_canvas = to_canvas(a, p, px, py, &cx, &cy);
    p->curx = cx; p->cury = cy;
    a->dirty = 1;
    if (p->color_dlg) { p_press(a, px, py, 1); return; }
    if (p->moving) { p->fx = cx - p->mdx; p->fy = cy - p->mdy; p->comp_dirty = 1; return; }
    if (!p->drawing) return;
    void (*st)(pt_t *, int, int) = 0;
    switch (p->tool) {
    case T_PENCIL: st = st_pencil; break;
    case T_BRUSH: st = st_brush; break;
    case T_ERASER: st = st_eraser; break;
    case T_AIR: st = st_air; break;
    case T_CALLI: st = st_calli; break;
    case T_MARKER: st = st_marker; break;
    }
    if (st) {
        if (p->tool == T_MARKER || p->tool == T_AIR) st(p, cx, cy);      /* these build up, a line would overpaint */
        else line_with(p, p->lx, p->ly, cx, cy, st);
        p->comp_dirty = 1;
    }
    p->lx = cx; p->ly = cy;
}

static void p_release(uiapp_t *a, int px, int py)
{
    pt_t *p = a->st;
    int cx, cy;
    to_canvas(a, p, px, py, &cx, &cy);
    if (p->moving) { p->moving = 0; return; }
    if (!p->drawing) return;
    p->drawing = 0;
    if (p->tool == T_SELECT) {
        int l = p->sx < cx ? p->sx : cx, t = p->sy < cy ? p->sy : cy;
        int r = p->sx < cx ? cx : p->sx, b = p->sy < cy ? cy : p->sy;
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r >= p->W) r = p->W - 1;
        if (b >= p->H) b = p->H - 1;
        if (r > l && b > t) { p->sel = 1; p->selx = l; p->sely = t; p->selw = r - l + 1; p->selh = b - t + 1; }
    } else if (p->tool >= T_LINE) {
        stamp_target = p;
        draw_shape(p, p->tool, p->sx, p->sy, cx, cy);
        p->comp_dirty = 1;
    }
    a->dirty = 1;
}

static void p_key(uiapp_t *a, int k)
{
    pt_t *p = a->st;
    a->dirty = 1;
    if (p->dlg.active) { if (ui_filedlg_key(&p->dlg, k) == 1) dlg_result(a, p); return; }
    if (p->typing) {
        int n = (int)strlen(p->text);
        if (k == '\n' || k == UI_KEY_ESC) { if (k == '\n') commit_text(p); else p->typing = 0; }
        else if (k == '\b') { if (n) p->text[n - 1] = 0; }
        else if (k >= 32 && k < 256 && n < 94) { p->text[n] = (char)k; p->text[n + 1] = 0; }
        return;
    }
    if (k == UI_KEY_ESC) { p->menu = 0; p->color_dlg = 0; drop_floating(p); p->sel = 0; }
    else if (k == 0x1A) { drop_floating(p); undo(p); }                   /* Ctrl+Z */
    else if (k == 0x19) { drop_floating(p); redo(p); }                   /* Ctrl+Y */
    else if (k == 0x03) copy_sel(p, 0);                                   /* Ctrl+C */
    else if (k == 0x18) copy_sel(p, 1);                                   /* Ctrl+X */
    else if (k == 0x16) paste(p);                                         /* Ctrl+V */
    else if (k == 0x13) save(a, p, 0);                                    /* Ctrl+S */
    else if (k == 0x01) menu_action(a, p, 2, 7);                          /* Ctrl+A */
    else if (k == UI_KEY_DEL) menu_action(a, p, 2, 8);
    else if (k == '+' || k == '=') set_zoom(p, p->zoom * 3 / 2);
    else if (k == '-') set_zoom(p, p->zoom * 2 / 3);
    else if (k == 'p') p->tool = T_PENCIL;
    else if (k == 'b') p->tool = T_BRUSH;
    else if (k == 'e') p->tool = T_ERASER;
    else if (k == 'f') p->tool = T_FILL;
    else if (k == 't') p->tool = T_TEXT;
    else if (k == 's') p->tool = T_SELECT;
    else if (k == 'x') { uint32_t t = p->c1; p->c1 = p->c2; p->c2 = t; }
    else if (k == '[' && p->size > 0) p->size--;
    else if (k == ']' && p->size < 5) p->size++;
    else if (k == UI_KEY_LEFT) p->panx += 40;
    else if (k == UI_KEY_RIGHT) p->panx -= 40;
    else if (k == UI_KEY_UP) p->pany += 40;
    else if (k == UI_KEY_DOWN) p->pany -= 40;
}

static void p_wheel(uiapp_t *a, int d) { pt_t *p = a->st; p->pany += d * 40; a->dirty = 1; }

static int p_tick(uiapp_t *a)
{
    pt_t *p = a->st;
    static uint32_t blink;
    uint32_t t = uihost_ms() / 500;
    if (p->typing && t != blink) { blink = t; return 1; }
    return 0;
}

static void p_init(uiapp_t *a, const char *arg)
{
    pt_t *p = a->st;
    p->c1 = 0x000000; p->c2 = 0xFFFFFF;
    p->size = 2; p->tool = T_BRUSH; p->zoom = 100;
    p->show_status = 1; p->show_layers = 1;
    p->rng = uihost_ms() | 1;
    new_canvas(p, 800, 500);
    if (arg && p->layer[0]) open_file(a, p, arg);
    else fit(a, p);
}

static void p_close(uiapp_t *a)
{
    pt_t *p = a->st;
    for (int l = 0; l < p->nl; l++) free(p->layer[l]);
    if (p->comp) free(p->comp);
    if (p->floatp) free(p->floatp);
    if (p->clip) free(p->clip);
    for (int i = 0; i < p->nundo; i++) snap_free(&p->undo[i]);
    for (int i = 0; i < p->nredo; i++) snap_free(&p->redo[i]);
}

const uiapp_class_t app_paint = {
    "paint", "Paint", "paint", 1060, 600, 1, sizeof(pt_t),
    p_init, p_draw, p_press, p_drag, p_release, p_key, p_wheel, p_tick, p_close,
};
