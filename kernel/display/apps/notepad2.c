#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/* Notepad: a plain text editor with line numbers, a toolbar and Open/Save dialogs. */

#define CAP_STEP 4096

typedef struct {
    char *buf;
    int len, cap;
    int cur;                     /* cursor, a byte offset into buf */
    int top, left;               /* first line and column shown */
    int modified;
    char path[224];
    char msg[64];
    uint32_t msg_ms;
    ui_filedlg_t dlg;
    int dlg_for;                 /* 1 open, 2 save */
    int want_col;                /* column kept while moving up and down */
} np_t;

#define BAR_H 44
#define STATUS_H 26
#define GUTTER 52
#define TX 8                     /* text inset */

static int ensure(np_t *n, int need)
{
    if (need <= n->cap) return 0;
    int cap = (need / CAP_STEP + 1) * CAP_STEP;
    char *b = malloc((size_t)cap);
    if (!b) return -1;
    if (n->buf) { memcpy(b, n->buf, (size_t)n->len); free(n->buf); }
    n->buf = b;
    n->cap = cap;
    return 0;
}

static void insert(np_t *n, const char *s, int k)
{
    if (ensure(n, n->len + k + 1) != 0) return;
    memmove(n->buf + n->cur + k, n->buf + n->cur, (size_t)(n->len - n->cur));
    memcpy(n->buf + n->cur, s, (size_t)k);
    n->len += k;
    n->cur += k;
    n->modified = 1;
}

static void erase(np_t *n, int at, int k)
{
    if (at < 0 || at + k > n->len || k <= 0) return;
    memmove(n->buf + at, n->buf + at + k, (size_t)(n->len - at - k));
    n->len -= k;
    if (n->cur > at) n->cur = n->cur >= at + k ? n->cur - k : at;
    n->modified = 1;
}

static int line_start(np_t *n, int pos) { while (pos > 0 && n->buf[pos - 1] != '\n') pos--; return pos; }
static int line_end(np_t *n, int pos) { while (pos < n->len && n->buf[pos] != '\n') pos++; return pos; }

static void cursor_lc(np_t *n, int *line, int *col)
{
    int l = 0, c = 0;
    for (int i = 0; i < n->cur; i++) { if (n->buf[i] == '\n') { l++; c = 0; } else c++; }
    *line = l; *col = c;
}

static int count_lines(np_t *n)
{
    int l = 1;
    for (int i = 0; i < n->len; i++) if (n->buf[i] == '\n') l++;
    return l;
}

static void say(np_t *n, const char *m) { strncpy(n->msg, m, sizeof(n->msg) - 1); n->msg_ms = uihost_ms(); }

static void load(np_t *n, const char *path)
{
    uint8_t *d; uint32_t len;
    if (uihost_read(path, &d, &len) != 0) { say(n, "Could not open that file"); return; }
    n->len = 0; n->cur = 0; n->top = 0; n->left = 0;
    if (ensure(n, (int)len + 1) == 0) {
        int o = 0;
        for (uint32_t i = 0; i < len; i++) if (d[i] != '\r') n->buf[o++] = (char)d[i];   /* CRLF files read as LF */
        n->len = o;
    }
    free(d);
    strncpy(n->path, path, sizeof(n->path) - 1);
    n->modified = 0;
}

static void save(np_t *n, uiapp_t *a)
{
    if (!n->path[0]) { ui_filedlg_open(&n->dlg, 1, 0, "Untitled.txt"); n->dlg_for = 2; return; }
    if (uihost_write(n->path, n->buf, (uint32_t)n->len) == 0) { n->modified = 0; say(n, "Saved"); }
    else say(n, "Could not save -- is the folder writable?");
    (void)a;
}

static void set_title(uiapp_t *a, np_t *n)
{
    strcpy(a->title, n->path[0] ? ui_basename(n->path) : "Untitled");
    if (n->modified) strcat(a->title, " \xE2\x80\xA2");          /* a dot for unsaved changes */
    strcat(a->title, " - Notepad");
}

static void np_init(uiapp_t *a, const char *arg)
{
    np_t *n = a->st;
    ensure(n, CAP_STEP);
    if (arg) load(n, arg);
    set_title(a, n);
}

static void np_close(uiapp_t *a) { np_t *n = a->st; if (n->buf) free(n->buf); }

static void keep_visible(np_t *n, int rows, int cols)
{
    int l, c;
    cursor_lc(n, &l, &c);
    if (l < n->top) n->top = l;
    if (l >= n->top + rows) n->top = l - rows + 1;
    if (c < n->left) n->left = c;
    if (c >= n->left + cols) n->left = c - cols + 1;
}

static void np_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    np_t *n = a->st;
    set_title(a, n);
    gfx_fill(s, x, y, w, BAR_H, UI_TOOLBAR);
    gfx_hline(s, x, y + BAR_H - 1, w, 0x000000, 120);
    static const char *const tb[3] = { "New", "Open", "Save" };
    for (int i = 0; i < 3; i++) ui_button(s, x + 10 + i * 78, y + 8, 70, 28, tb[i], UI_CARD2, UI_TEXT, 0);
    gfx_surface_t t = *s;
    gfx_clip(&t, x + 260, y, w - 270, BAR_H);
    gfx_text(&t, &ui_font_regular, x + 262, y + 13, n->path[0] ? n->path : "Not saved yet", UI_DIM, 255);

    int ty = y + BAR_H, th = h - BAR_H - STATUS_H;
    gfx_fill(s, x, ty, GUTTER, th, UI_SIDEBAR);
    gfx_fill(s, x + GUTTER, ty, w - GUTTER, th, UI_BG);
    int rows = (th - 8) / UI_CELL_H, cols = (w - GUTTER - TX * 2) / UI_CELL_W;
    if (cols < 1) cols = 1;
    keep_visible(n, rows, cols);
    int cl, cc;
    cursor_lc(n, &cl, &cc);
    /* walk to the first line shown */
    int pos = 0, line = 0;
    while (line < n->top && pos < n->len) { if (n->buf[pos] == '\n') line++; pos++; }
    gfx_surface_t tc = *s;
    gfx_clip(&tc, x + GUTTER, ty, w - GUTTER, th);
    int total = count_lines(n);
    for (int r = 0; r < rows && line < total; r++) {
        int ly = ty + 4 + r * UI_CELL_H;
        if (line == cl) gfx_fill(&tc, x + GUTTER, ly, w - GUTTER, UI_CELL_H, UI_CARD);
        char num[12] = "";
        ui_num(num, line + 1);
        gfx_text(s, &ui_font_small, x + GUTTER - 10 - gfx_text_width(&ui_font_small, num), ly + 2, num, line == cl ? UI_TEXT : UI_DIM, 255);
        int col = 0;
        while (pos < n->len && n->buf[pos] != '\n') {
            if (col >= n->left && col < n->left + cols) {
                uint8_t ch = (uint8_t)n->buf[pos];
                gfx_cell(&tc, x + GUTTER + TX + (col - n->left) * UI_CELL_W, ly, ch < 32 ? ' ' : ch, UI_TEXT,
                         line == cl ? UI_CARD : UI_BG);
            }
            col++; pos++;
        }
        if (line == cl && cc >= n->left && cc < n->left + cols)
            gfx_fill(&tc, x + GUTTER + TX + (cc - n->left) * UI_CELL_W, ly + 1, 2, UI_CELL_H - 2, UI_ACCENT);
        if (pos < n->len) pos++;            /* the newline */
        line++;
    }

    int sy = y + h - STATUS_H;
    gfx_fill(s, x, sy, w, STATUS_H, UI_TOOLBAR);
    char st[96] = "Ln ";
    ui_num(st, cl + 1); strcat(st, ", Col "); ui_num(st, cc + 1);
    strcat(st, "    "); ui_num(st, count_lines(n)); strcat(st, " lines    "); ui_size(st, (uint32_t)n->len);
    gfx_text(s, &ui_font_small, x + 12, sy + 5, st, UI_DIM, 255);
    if (n->msg[0] && uihost_ms() - n->msg_ms < 3000)
        gfx_text(s, &ui_font_small, x + w - 12 - gfx_text_width(&ui_font_small, n->msg), sy + 5, n->msg, 0x86EFAC, 255);
    ui_filedlg_draw(&n->dlg, s, x, y, w, h);
}

static void dlg_done(uiapp_t *a, np_t *n)
{
    if (n->dlg_for == 1) load(n, n->dlg.result);
    else if (n->dlg_for == 2) { strncpy(n->path, n->dlg.result, sizeof(n->path) - 1); save(n, a); }
    n->dlg_for = 0;
}

static void np_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    np_t *n = a->st;
    a->dirty = 1;
    if (n->dlg.active) {
        int r = ui_filedlg_press(&n->dlg, px, py, a->w, a->h);
        if (r == 1) dlg_done(a, n);
        return;
    }
    if (py < BAR_H) {
        int i = (px - 10) / 78;
        if (px < 10 || i > 2 || (px - 10) % 78 > 70) return;
        if (i == 0) { n->len = 0; n->cur = 0; n->path[0] = 0; n->modified = 0; n->top = n->left = 0; }
        else if (i == 1) { ui_filedlg_open(&n->dlg, 0, 0, 0); n->dlg_for = 1; }
        else save(n, a);
        return;
    }
    /* put the cursor where the click was */
    int line = n->top + (py - BAR_H - 4) / UI_CELL_H;
    int col = n->left + (px - GUTTER - TX + UI_CELL_W / 2) / UI_CELL_W;
    if (col < 0) col = 0;
    int pos = 0, l = 0;
    while (l < line && pos < n->len) { if (n->buf[pos] == '\n') l++; pos++; }
    int end = line_end(n, pos);
    n->cur = pos + col < end ? pos + col : end;
    n->want_col = -1;
}

static void np_key(uiapp_t *a, int k)
{
    np_t *n = a->st;
    a->dirty = 1;
    if (n->dlg.active) {
        int r = ui_filedlg_key(&n->dlg, k);
        if (r == 1) dlg_done(a, n);
        return;
    }
    if (k == 0x13) { save(n, a); return; }                 /* Ctrl+S */
    if (k == 0x0F) { ui_filedlg_open(&n->dlg, 0, 0, 0); n->dlg_for = 1; return; }   /* Ctrl+O */
    if (k == UI_KEY_LEFT) { if (n->cur > 0) n->cur--; n->want_col = -1; }
    else if (k == UI_KEY_RIGHT) { if (n->cur < n->len) n->cur++; n->want_col = -1; }
    else if (k == UI_KEY_UP || k == UI_KEY_DOWN) {
        int ls = line_start(n, n->cur);
        int col = n->want_col >= 0 ? n->want_col : n->cur - ls;
        n->want_col = col;
        int target;
        if (k == UI_KEY_UP) { if (ls == 0) return; target = line_start(n, ls - 1); }
        else { int le = line_end(n, n->cur); if (le >= n->len) return; target = le + 1; }
        int te = line_end(n, target);
        n->cur = target + col < te ? target + col : te;
    }
    else if (k == '\b') { if (n->cur > 0) erase(n, n->cur - 1, 1); }
    else if (k == UI_KEY_DEL) { if (n->cur < n->len) erase(n, n->cur, 1); }
    else if (k == '\t') insert(n, "    ", 4);
    else if (k == '\n' || (k >= 32 && k < 256 && k != 127)) { char c = (char)k; insert(n, &c, 1); n->want_col = -1; }
}

static void np_wheel(uiapp_t *a, int d)
{
    np_t *n = a->st;
    n->top -= d * 3;
    if (n->top < 0) n->top = 0;
    int max = count_lines(n) - 1;
    if (n->top > max) n->top = max;
    /* keep the cursor inside what is shown, so drawing does not scroll back to it */
    int rows = (a->h - BAR_H - STATUS_H - 8) / UI_CELL_H;
    int l, c;
    cursor_lc(n, &l, &c);
    if (l < n->top || l >= n->top + rows) {
        int pos = 0, line = 0, want = l < n->top ? n->top : n->top + rows - 1;
        while (line < want && pos < n->len) { if (n->buf[pos] == '\n') line++; pos++; }
        n->cur = pos;
    }
    a->dirty = 1;
}

static int np_tick(uiapp_t *a)
{
    np_t *n = a->st;
    if (n->msg[0] && uihost_ms() - n->msg_ms >= 3000 && uihost_ms() - n->msg_ms < 3100) return 1;
    (void)a;
    return 0;
}

const uiapp_class_t app_notepad = {
    "notepad", "Notepad", "notepad", 760, 470, 1, sizeof(np_t),
    np_init, np_draw, np_press, 0, 0, np_key, np_wheel, np_tick, np_close,
};
