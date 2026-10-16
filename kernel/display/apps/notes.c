#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/* Sticky Notes: coloured notes on a board, typed into directly, kept in ~/.notes. */

#define MAXN 12
#define NOTE_LEN 240
#define NW 200
#define NH 170

typedef struct {
    char text[MAXN][NOTE_LEN];
    uint8_t color[MAXN];
    int n, edit;
    uint32_t blink_ms;
    int saved;
} notes_t;

static const uint32_t ncol[5] = { 0xFDE68A, 0xBBF7D0, 0xBFDBFE, 0xFBCFE8, 0xFED7AA };

static void pref(char *o)
{
    strcpy(o, uihost_home());
    if (strcmp(o, "/")) strcat(o, "/");
    strcat(o, ".notes");
}

/* the file: one note per record, records split by 0x1E, the first byte of each its colour */
static void save(notes_t *n)
{
    static char buf[MAXN * (NOTE_LEN + 2)];
    int o = 0;
    for (int i = 0; i < n->n; i++) {
        buf[o++] = (char)('0' + n->color[i]);
        int l = (int)strlen(n->text[i]);
        memcpy(buf + o, n->text[i], (size_t)l);
        o += l;
        buf[o++] = 0x1E;
    }
    char p[96];
    pref(p);
    uihost_write(p, buf, (uint32_t)o);
    n->saved = 1;
}

static void load(notes_t *n)
{
    char p[96];
    pref(p);
    uint8_t *d; uint32_t len;
    n->n = 0;
    if (uihost_read(p, &d, &len) == 0) {
        uint32_t i = 0;
        while (i < len && n->n < MAXN) {
            int c = d[i] >= '0' && d[i] <= '4' ? d[i] - '0' : 0;
            i++;
            int o = 0;
            while (i < len && d[i] != 0x1E) { if (o < NOTE_LEN - 1) n->text[n->n][o++] = (char)d[i]; i++; }
            n->text[n->n][o] = 0;
            n->color[n->n] = (uint8_t)c;
            n->n++;
            i++;
        }
        free(d);
    }
    if (!n->n) {
        strcpy(n->text[0], "Welcome to Sticky Notes.\n\nClick a note to type in it. + adds one, the x on a note removes it.");
        n->color[0] = 0;
        n->n = 1;
    }
}

static void note_pos(uiapp_t *a, int i, int *x, int *y)
{
    int cols = (a->w - 20) / (NW + 16);
    if (cols < 1) cols = 1;
    *x = 16 + (i % cols) * (NW + 16);
    *y = 56 + (i / cols) * (NH + 16);
}

static void n_init(uiapp_t *a, const char *arg) { (void)arg; notes_t *n = a->st; n->edit = -1; load(n); }

static void n_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    notes_t *n = a->st;
    gfx_fill(s, x, y, w, h, 0x2B2A27);
    gfx_text(s, &ui_font_title, x + 16, y + 12, "Notes", UI_TEXT, 255);
    ui_button(s, x + w - 56, y + 12, 40, 30, "+", UI_CARD2, UI_TEXT, 0);
    for (int i = 0; i < n->n; i++) {
        int nx, ny;
        note_pos(a, i, &nx, &ny);
        nx += x; ny += y;
        gfx_shadow(s, nx, ny, NW, NH, 140);
        gfx_rrect(s, nx, ny, NW, NH, 6, ncol[n->color[i] % 5], 255);
        gfx_fill_alpha(s, nx, ny, NW, 22, 0x000000, 18);
        for (int c = 0; c < 5; c++) gfx_circle(s, nx + 12 + c * 14, ny + 11, 4, ncol[c], 255);
        gfx_text(s, &ui_font_medium, nx + NW - 20, ny + 2, "\xC3\x97", 0x55504A, 255);
        /* the text, wrapped to the note */
        gfx_surface_t c = *s;
        gfx_clip(&c, nx + 8, ny + 26, NW - 16, NH - 32);
        int lx = nx + 10, ly = ny + 28;
        const char *p = n->text[i];
        char word[64];
        while (*p) {
            if (*p == '\n') { lx = nx + 10; ly += 18; p++; continue; }
            int k = 0;
            while (p[k] && p[k] != ' ' && p[k] != '\n' && k < 63) { word[k] = p[k]; k++; }
            word[k] = 0;
            int ww = gfx_text_width(&ui_font_regular, word);
            if (lx + ww > nx + NW - 10 && lx > nx + 10) { lx = nx + 10; ly += 18; }
            gfx_text(&c, &ui_font_regular, lx, ly, word, 0x2A2622, 255);
            lx += ww;
            p += k;
            if (*p == ' ') { lx += 4; p++; }
        }
        if (i == n->edit && (uihost_ms() / 500) % 2 == 0) gfx_fill(&c, lx + 1, ly + 1, 2, 16, 0x2A2622);
        if (i == n->edit) gfx_rrect_outline(s, nx - 2, ny - 2, NW + 4, NH + 4, 8, UI_ACCENT, 255);
    }
    (void)h;
}

static void n_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    notes_t *n = a->st;
    a->dirty = 1;
    if (ui_hit(px, py, a->w - 56, 12, 40, 30) && n->n < MAXN) {
        n->text[n->n][0] = 0;
        n->color[n->n] = (uint8_t)(n->n % 5);
        n->edit = n->n++;
        save(n);
        return;
    }
    for (int i = 0; i < n->n; i++) {
        int nx, ny;
        note_pos(a, i, &nx, &ny);
        if (!ui_hit(px, py, nx, ny, NW, NH)) continue;
        if (py < ny + 22) {
            if (px > nx + NW - 24) {                       /* remove */
                for (int k = i; k < n->n - 1; k++) { strcpy(n->text[k], n->text[k + 1]); n->color[k] = n->color[k + 1]; }
                n->n--;
                n->edit = -1;
                save(n);
                return;
            }
            for (int c = 0; c < 5; c++) if (px >= nx + 6 + c * 14 && px < nx + 18 + c * 14) { n->color[i] = (uint8_t)c; save(n); }
        }
        n->edit = i;
        return;
    }
    n->edit = -1;
}

static void n_key(uiapp_t *a, int k)
{
    notes_t *n = a->st;
    if (n->edit < 0) return;
    char *t = n->text[n->edit];
    int l = (int)strlen(t);
    if (k == '\b') { if (l) t[l - 1] = 0; }
    else if (k == UI_KEY_ESC) n->edit = -1;
    else if ((k == '\n' || (k >= 32 && k < 256 && k != 127)) && l < NOTE_LEN - 1) { t[l] = (char)k; t[l + 1] = 0; }
    save(n);
    a->dirty = 1;
}

static int n_tick(uiapp_t *a)
{
    notes_t *n = a->st;
    if (n->edit < 0) return 0;
    uint32_t phase = uihost_ms() / 500;
    if (phase != n->blink_ms) { n->blink_ms = phase; return 1; }
    return 0;
}

const uiapp_class_t app_notes = {
    "notes", "Sticky Notes", "sticky", 680, 460, 1, sizeof(notes_t),
    n_init, n_draw, n_press, 0, 0, n_key, 0, n_tick, 0,
};
