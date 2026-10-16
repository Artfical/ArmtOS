#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"

/* 2048, with tiles that look like the original's. */

typedef struct {
    int g[4][4];
    int score, best, over, won;
    uint32_t rng;
} g2048_t;

static uint32_t rnd(g2048_t *g) { g->rng = g->rng * 1103515245u + 12345u; return g->rng >> 16; }

static void spawn(g2048_t *g)
{
    int free_n = 0;
    for (int i = 0; i < 16; i++) if (!g->g[i / 4][i % 4]) free_n++;
    if (!free_n) return;
    int k = (int)(rnd(g) % (uint32_t)free_n);
    for (int i = 0; i < 16; i++)
        if (!g->g[i / 4][i % 4] && k-- == 0) { g->g[i / 4][i % 4] = rnd(g) % 10 ? 2 : 4; return; }
}

static void reset(g2048_t *g)
{
    int best = g->best;
    uint32_t r = g->rng ^ uihost_ms();
    memset(g, 0, sizeof(*g));
    g->best = best;
    g->rng = r ? r : 1;
    spawn(g); spawn(g);
}

/* slides one line towards index 0; returns 1 if anything moved */
static int slide(int *v[4], g2048_t *g)
{
    int out[4] = { 0 }, n = 0, moved = 0, merged = 0;
    for (int i = 0; i < 4; i++) {
        int x = *v[i];
        if (!x) continue;
        if (n && out[n - 1] == x && !merged) { out[n - 1] *= 2; g->score += out[n - 1]; if (out[n - 1] == 2048) g->won = 1; merged = 1; }
        else { out[n++] = x; merged = 0; }
    }
    for (int i = 0; i < 4; i++) { if (*v[i] != out[i]) moved = 1; *v[i] = out[i]; }
    return moved;
}

static int can_move(g2048_t *g)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!g->g[r][c]) return 1;
            if (c < 3 && g->g[r][c] == g->g[r][c + 1]) return 1;
            if (r < 3 && g->g[r][c] == g->g[r + 1][c]) return 1;
        }
    return 0;
}

static void move(g2048_t *g, int dir)
{
    if (g->over) return;
    int moved = 0;
    for (int i = 0; i < 4; i++) {
        int *v[4];
        for (int j = 0; j < 4; j++) {
            switch (dir) {
            case 0: v[j] = &g->g[i][j]; break;           /* left */
            case 1: v[j] = &g->g[i][3 - j]; break;       /* right */
            case 2: v[j] = &g->g[j][i]; break;           /* up */
            default: v[j] = &g->g[3 - j][i]; break;      /* down */
            }
        }
        moved |= slide(v, g);
    }
    if (moved) spawn(g);
    if (g->score > g->best) g->best = g->score;
    if (!can_move(g)) g->over = 1;
}

static uint32_t tile_bg(int v)
{
    switch (v) {
    case 0: return 0xCDC1B4;  case 2: return 0xEEE4DA;  case 4: return 0xEDE0C8;  case 8: return 0xF2B179;
    case 16: return 0xF59563; case 32: return 0xF67C5F; case 64: return 0xF65E3B; case 128: return 0xEDCF72;
    case 256: return 0xEDCC61; case 512: return 0xEDC850; case 1024: return 0xEDC53F; default: return 0xEDC22E;
    }
}

#define CELL 74
#define GAP2 12

static void g_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    g2048_t *g = a->st;
    gfx_fill(s, x, y, w, h, 0xFAF8EF);
    gfx_text(s, &ui_font_huge, x + 20, y + 10, "2048", 0x776E65, 255);
    for (int k = 0; k < 2; k++) {
        int bx = x + w - 20 - (2 - k) * 96 + 6;
        gfx_rrect(s, bx, y + 18, 88, 50, 6, 0xBBADA0, 255);
        gfx_text_center(s, &ui_font_small, bx, y + 22, 88, k ? "BEST" : "SCORE", 0xEEE4DA, 255);
        char n[16] = "";
        ui_num(n, k ? g->best : g->score);
        gfx_text_center(s, &ui_font_medium, bx, y + 40, 88, n, 0xFFFFFF, 255);
    }
    int bx = x + (w - (4 * CELL + 5 * GAP2)) / 2, by = y + 90;
    gfx_rrect(s, bx, by, 4 * CELL + 5 * GAP2, 4 * CELL + 5 * GAP2, 8, 0xBBADA0, 255);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int v = g->g[r][c];
            int tx = bx + GAP2 + c * (CELL + GAP2), ty = by + GAP2 + r * (CELL + GAP2);
            gfx_rrect(s, tx, ty, CELL, CELL, 6, tile_bg(v), 255);
            if (!v) continue;
            char n[8] = "";
            ui_num(n, v);
            const ui_font_t *f = v < 128 ? &ui_font_huge : &ui_font_title;
            gfx_text_center(s, f, tx, ty + (CELL - f->line_h) / 2 + 2, CELL, n, v <= 4 ? 0x776E65 : 0xF9F6F2, 255);
        }
    if (g->over || g->won) {
        gfx_rrect(s, bx, by, 4 * CELL + 5 * GAP2, 4 * CELL + 5 * GAP2, 8, 0xFAF8EF, 170);
        gfx_text_center(s, &ui_font_huge, bx, by + 120, 4 * CELL + 5 * GAP2, g->won ? "You win!" : "Game over", 0x776E65, 255);
        gfx_text_center(s, &ui_font_regular, bx, by + 190, 4 * CELL + 5 * GAP2, "Press Enter or click for a new game", 0x776E65, 255);
    }
    gfx_text_center(s, &ui_font_small, x, y + h - 26, w, "Arrow keys or W A S D to slide the tiles", 0x8F7A66, 255);
}

static void g_key(uiapp_t *a, int k)
{
    g2048_t *g = a->st;
    if (k == UI_KEY_LEFT || k == 'a') move(g, 0);
    else if (k == UI_KEY_RIGHT || k == 'd') move(g, 1);
    else if (k == UI_KEY_UP || k == 'w') move(g, 2);
    else if (k == UI_KEY_DOWN || k == 's') move(g, 3);
    else if (k == '\n' || k == 'n') reset(g);
    a->dirty = 1;
}

static void g_press(uiapp_t *a, int x, int y, int b)
{
    (void)x; (void)y; (void)b;
    g2048_t *g = a->st;
    if (g->over || g->won) { reset(g); a->dirty = 1; }
}

static void g_init(uiapp_t *a, const char *arg) { (void)arg; ((g2048_t *)a->st)->rng = uihost_ms() | 1; reset(a->st); }

const uiapp_class_t app_2048 = {
    "2048", "2048", "2048", 4 * CELL + 5 * GAP2 + 40, 4 * CELL + 5 * GAP2 + 140, 0, sizeof(g2048_t),
    g_init, g_draw, g_press, 0, 0, g_key, 0, 0, 0,
};
