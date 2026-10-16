#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/* Activity: the running tasks with their CPU share, memory in use, and a CPU history graph. */

#define MAXT 48
#define HIST 120

typedef struct {
    uihost_task_t t[MAXT];
    uint32_t prev_cpu[MAXT], prev_pid[MAXT];
    int pct[MAXT];
    int n, nprev, sel, scroll;
    uint32_t last_ms, last_total;
    uint8_t cpu_hist[HIST], mem_hist[HIST];
    int hist_n;
    char msg[64];
    int confirm;
} act_t;

static void sample(act_t *a)
{
    uint32_t now = uihost_ms();
    uint32_t dt = now - a->last_ms;
    a->last_ms = now;
    uihost_task_t cur[MAXT];
    int n = uihost_tasks(cur, MAXT);
    int busy = 0;
    for (int i = 0; i < n; i++) {
        uint32_t prev = cur[i].cpu;
        for (int j = 0; j < a->nprev; j++) if (a->prev_pid[j] == cur[i].pid) { prev = a->prev_cpu[j]; break; }
        uint32_t ticks = cur[i].cpu - prev;                /* scheduler ticks, 10 ms each */
        int p = dt ? (int)(ticks * 1000 / dt) : 0;          /* tenths of a percent */
        if (p > 1000) p = 1000;
        a->pct[i] = p;
        if (strcmp(cur[i].name, "idle")) busy += p;
    }
    for (int i = 0; i < n; i++) { a->prev_pid[i] = cur[i].pid; a->prev_cpu[i] = cur[i].cpu; a->t[i] = cur[i]; }
    a->nprev = a->n = n;
    if (busy > 1000) busy = 1000;
    uint32_t tot, used;
    uihost_memory(&tot, &used);
    if (a->hist_n == HIST) { memmove(a->cpu_hist, a->cpu_hist + 1, HIST - 1); memmove(a->mem_hist, a->mem_hist + 1, HIST - 1); a->hist_n--; }
    a->cpu_hist[a->hist_n] = (uint8_t)(busy / 10);
    a->mem_hist[a->hist_n] = (uint8_t)(tot ? used * 100 / tot : 0);
    a->hist_n++;
    if (a->sel >= a->n) a->sel = a->n - 1;
}

static void act_init(uiapp_t *ap, const char *arg) { (void)arg; act_t *a = ap->st; a->sel = -1; a->last_ms = uihost_ms(); sample(a); }

static int act_tick(uiapp_t *ap)
{
    act_t *a = ap->st;
    if (uihost_ms() - a->last_ms >= 1000) { sample(a); return 1; }
    return 0;
}

static void graph(gfx_surface_t *s, int x, int y, int w, int h, const uint8_t *v, int n, uint32_t col, const char *label, int now)
{
    gfx_rrect(s, x, y, w, h, 10, UI_CARD, 255);
    gfx_text(s, &ui_font_small, x + 12, y + 8, label, UI_DIM, 255);
    char pc[8] = "";
    ui_num(pc, now); strcat(pc, "%");
    gfx_text(s, &ui_font_title, x + w - 12 - gfx_text_width(&ui_font_title, pc), y + 6, pc, UI_TEXT, 255);
    int gx = x + 12, gy = y + 40, gw = w - 24, gh = h - 50;
    for (int k = 1; k < 4; k++) gfx_hline(s, gx, gy + gh * k / 4, gw, 0xFFFFFF, 14);
    for (int i = 0; i < n; i++) {
        int bx = gx + gw - (n - i) * gw / HIST;
        int bh = v[i] * gh / 100;
        gfx_fill_alpha(s, bx, gy + gh - bh, gw / HIST + 1, bh, col, 110);
        gfx_fill(s, bx, gy + gh - bh, gw / HIST + 1, 2, col);
    }
}

static void act_draw(uiapp_t *ap, gfx_surface_t *s, int x, int y, int w, int h)
{
    act_t *a = ap->st;
    int gw = (w - 36) / 2;
    graph(s, x + 12, y + 12, gw, 120, a->cpu_hist, a->hist_n, UI_ACCENT, "CPU", a->hist_n ? a->cpu_hist[a->hist_n - 1] : 0);
    uint32_t tot, used;
    uihost_memory(&tot, &used);
    char ml[48] = "MEMORY  ";
    ui_size(ml, used * 1024); strcat(ml, " of "); ui_size(ml, tot * 1024);
    graph(s, x + 24 + gw, y + 12, gw, 120, a->mem_hist, a->hist_n, UI_GREEN, ml, a->hist_n ? a->mem_hist[a->hist_n - 1] : 0);

    int ty = y + 146;
    gfx_rrect(s, x + 12, ty, w - 24, h - ty + y - 56, 10, UI_CARD, 255);
    gfx_text(s, &ui_font_small, x + 28, ty + 10, "PID", UI_DIM, 255);
    gfx_text(s, &ui_font_small, x + 90, ty + 10, "NAME", UI_DIM, 255);
    gfx_text(s, &ui_font_small, x + w - 250, ty + 10, "STATE", UI_DIM, 255);
    gfx_text(s, &ui_font_small, x + w - 120, ty + 10, "CPU", UI_DIM, 255);
    gfx_hline(s, x + 20, ty + 32, w - 40, 0xFFFFFF, 20);
    int rows = (h - (ty - y) - 56 - 40) / 26;
    for (int r = 0; r < rows && a->scroll + r < a->n; r++) {
        int i = a->scroll + r, ry = ty + 38 + r * 26;
        if (i == a->sel) gfx_rrect(s, x + 18, ry - 3, w - 36, 25, 6, 0x2E4A7A, 255);
        char t[16] = "";
        ui_num(t, a->t[i].pid);
        gfx_text(s, &ui_font_regular, x + 28, ry, t, UI_TEXT, 255);
        gfx_text(s, &ui_font_regular, x + 90, ry, a->t[i].name, UI_TEXT, 255);
        static const char *st[4] = { "ready", "running", "sleeping", "blocked" };
        gfx_text(s, &ui_font_regular, x + w - 250, ry, a->t[i].state >= 0 && a->t[i].state < 4 ? st[a->t[i].state] : "?", UI_DIM, 255);
        t[0] = 0;
        ui_num(t, a->pct[i] / 10); strcat(t, "."); ui_num(t, a->pct[i] % 10); strcat(t, "%");
        int bw = a->pct[i] * 50 / 1000;
        gfx_rrect(s, x + w - 120, ry + 5, 52, 8, 4, 0x3A3C46, 255);
        if (bw) gfx_rrect(s, x + w - 120, ry + 5, bw + 2, 8, 4, UI_ACCENT, 255);
        gfx_text(s, &ui_font_small, x + w - 62, ry + 1, t, UI_TEXT, 255);
    }
    int by = y + h - 46;
    ui_button(s, x + w - 152, by, 140, 32, a->confirm ? "Really end it?" : "End Task", a->sel >= 0 ? 0x7F1D1D : UI_CARD2,
              a->sel >= 0 ? 0xFCA5A5 : 0x5C5F6A, 0);
    char inf[64] = "";
    ui_num(inf, a->n); strcat(inf, " tasks");
    if (a->msg[0]) { strcat(inf, "   "); strcat(inf, a->msg); }
    gfx_text(s, &ui_font_small, x + 16, by + 9, inf, UI_DIM, 255);
}

static void act_press(uiapp_t *ap, int px, int py, int b)
{
    (void)b;
    act_t *a = ap->st;
    ap->dirty = 1;
    if (ui_hit(px, py, ap->w - 152, ap->h - 46, 140, 32) && a->sel >= 0) {
        if (!a->confirm) { a->confirm = 1; return; }
        a->confirm = 0;
        strcpy(a->msg, uihost_kill(a->t[a->sel].pid) == 0 ? "ended" : "could not end it");
        sample(a);
        return;
    }
    a->confirm = 0;
    int ty = 146 + 38;
    if (py >= ty) {
        int i = a->scroll + (py - ty + 3) / 26;
        a->sel = i < a->n ? i : -1;
        a->msg[0] = 0;
    }
}

static void act_wheel(uiapp_t *ap, int d)
{
    act_t *a = ap->st;
    a->scroll -= d;
    if (a->scroll > a->n - 3) a->scroll = a->n - 3;
    if (a->scroll < 0) a->scroll = 0;
    ap->dirty = 1;
}

static void act_key(uiapp_t *ap, int k)
{
    act_t *a = ap->st;
    if (k == UI_KEY_DOWN && a->sel + 1 < a->n) a->sel++;
    else if (k == UI_KEY_UP && a->sel > 0) a->sel--;
    ap->dirty = 1;
}

const uiapp_class_t app_activity = {
    "taskmgr", "Activity", "taskmgr", 640, 520, 1, sizeof(act_t),
    act_init, act_draw, act_press, 0, 0, act_key, act_wheel, act_tick, 0,
};
