#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/* Clock: an analog face, the time and date, and a stopwatch. */

typedef struct {
    int tab;                     /* 0 clock, 1 stopwatch */
    int last_sec;
    int running;
    uint32_t sw_start, sw_acc;   /* stopwatch: started at, and time banked before that */
    uint32_t laps[6];
    int nlaps;
} clock_t_;

static const char *const wd[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mn[12] = { "January", "February", "March", "April", "May", "June", "July",
                                    "August", "September", "October", "November", "December" };

static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}

static void two(char *o, int v) { int n = (int)strlen(o); o[n] = (char)('0' + v / 10 % 10); o[n + 1] = (char)('0' + v % 10); o[n + 2] = 0; }

static void hand(gfx_surface_t *s, int cx, int cy, int deg, int len, int back, int r4, uint32_t c)
{
    int ex = cx + gfx_sin(deg) * len / 4096, ey = cy - gfx_cos(deg) * len / 4096;
    int bx = cx - gfx_sin(deg) * back / 4096, by = cy + gfx_cos(deg) * back / 4096;
    gfx_capsule(s, bx, by, ex, ey, r4, c, 255);
}

static void fmt_sw(char *o, uint32_t ms)
{
    o[0] = 0;
    two(o, (int)(ms / 60000)); strcat(o, ":");
    two(o, (int)(ms / 1000 % 60)); strcat(o, ".");
    two(o, (int)(ms / 10 % 100));
}

static uint32_t sw_now(clock_t_ *c) { return c->sw_acc + (c->running ? uihost_ms() - c->sw_start : 0); }

static void clock_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    clock_t_ *c = a->st;
    gfx_fill(s, x, y, w, h, UI_BG);
    /* tabs */
    int tw = 220, tx = x + (w - tw) / 2;
    gfx_rrect(s, tx, y + 14, tw, 30, 9, UI_CARD, 255);
    gfx_rrect(s, tx + 3 + c->tab * (tw / 2), y + 17, tw / 2 - 6, 24, 7, UI_LINE, 255);
    gfx_text_center(s, &ui_font_medium, tx, y + 20, tw / 2, "Clock", UI_TEXT, 255);
    gfx_text_center(s, &ui_font_medium, tx + tw / 2, y + 20, tw / 2, "Stopwatch", UI_TEXT, 255);

    int yr, mo, dy, hh, mm, ss;
    uihost_time(&yr, &mo, &dy, &hh, &mm, &ss);
    if (c->tab == 0) {
        int cx = x + w / 2, cy = y + 60 + 120, R = 110;
        gfx_circle(s, cx, cy, R + 6, 0x111216, 255);
        gfx_circle(s, cx, cy, R, 0xF4F4F6, 255);
        for (int i = 0; i < 60; i++) {
            int big = i % 5 == 0;
            int r0 = R - (big ? 16 : 8), r1 = R - 5;
            gfx_capsule(s, cx + gfx_sin(i * 6) * r0 / 4096, cy - gfx_cos(i * 6) * r0 / 4096,
                        cx + gfx_sin(i * 6) * r1 / 4096, cy - gfx_cos(i * 6) * r1 / 4096, big ? 6 : 3, big ? 0x222326 : 0x9A9CA4, 255);
        }
        static const char *num[4] = { "12", "3", "6", "9" };
        for (int i = 0; i < 4; i++) {
            int nx = cx + gfx_sin(i * 90) * (R - 32) / 4096, ny = cy - gfx_cos(i * 90) * (R - 32) / 4096;
            gfx_text_center(s, &ui_font_title, nx - 20, ny - 15, 40, num[i], 0x222326, 255);
        }
        hand(s, cx, cy, (hh % 12) * 30 + mm / 2, R - 50, 10, 14, 0x1F2024);
        hand(s, cx, cy, mm * 6 + ss / 10, R - 22, 12, 10, 0x1F2024);
        hand(s, cx, cy, ss * 6, R - 14, 20, 4, UI_ORANGE);
        gfx_circle(s, cx, cy, 6, UI_ORANGE, 255);
        gfx_circle(s, cx, cy, 2, 0xFFFFFF, 255);

        char t[16] = "";
        two(t, hh); strcat(t, ":"); two(t, mm); strcat(t, ":"); two(t, ss);
        gfx_text_center(s, &ui_font_huge, x, cy + R + 22, w, t, UI_TEXT, 255);
        char d[48] = "";
        strcpy(d, wd[weekday(yr, mo, dy)]);
        strcat(d, ", ");
        ui_num(d, dy); strcat(d, " "); strcat(d, mn[(mo + 11) % 12]); strcat(d, " "); ui_num(d, yr);
        gfx_text_center(s, &ui_font_regular, x, cy + R + 84, w, d, UI_DIM, 255);
    } else {
        char t[24];
        fmt_sw(t, sw_now(c));
        gfx_text_center(s, &ui_font_huge, x, y + 110, w, t, UI_TEXT, 255);
        int bw = 120, by = y + 200;
        ui_button(s, x + w / 2 - bw - 10, by, bw, 40, c->running ? "Lap" : "Reset", UI_CARD2, UI_TEXT, 0);
        ui_button(s, x + w / 2 + 10, by, bw, 40, c->running ? "Stop" : "Start", c->running ? 0x7F1D1D : 0x14532D,
                  c->running ? 0xFCA5A5 : 0x86EFAC, 0);
        for (int i = 0; i < c->nlaps; i++) {
            int ly = by + 64 + i * 30;
            gfx_hline(s, x + 40, ly - 6, w - 80, UI_LINE, 255);
            char l[16] = "Lap ";
            ui_num(l, c->nlaps - i);
            gfx_text(s, &ui_font_regular, x + 48, ly, l, UI_DIM, 255);
            fmt_sw(t, c->laps[i]);
            gfx_text(s, &ui_font_regular, x + w - 48 - gfx_text_width(&ui_font_regular, t), ly, t, UI_TEXT, 255);
        }
    }
}

static void clock_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    clock_t_ *c = a->st;
    int tw = 220, tx = (a->w - tw) / 2;
    if (ui_hit(px, py, tx, 14, tw, 30)) { c->tab = px >= tx + tw / 2; a->dirty = 1; return; }
    if (c->tab == 1) {
        int bw = 120, by = 200;
        if (ui_hit(px, py, a->w / 2 - bw - 10, by, bw, 40)) {
            if (c->running) {
                if (c->nlaps < 6) c->nlaps++;
                for (int i = c->nlaps - 1; i > 0; i--) c->laps[i] = c->laps[i - 1];
                c->laps[0] = sw_now(c);
            } else { c->sw_acc = 0; c->nlaps = 0; }
        } else if (ui_hit(px, py, a->w / 2 + 10, by, bw, 40)) {
            if (c->running) { c->sw_acc = sw_now(c); c->running = 0; }
            else { c->sw_start = uihost_ms(); c->running = 1; }
        }
        a->dirty = 1;
    }
}

static void clock_key(uiapp_t *a, int k)
{
    clock_t_ *c = a->st;
    if (k == '\t') c->tab ^= 1;
    else if (k == ' ' && c->tab == 1) clock_press(a, a->w / 2 + 20, 210, 1);
    a->dirty = 1;
}

static int clock_tick(uiapp_t *a)
{
    clock_t_ *c = a->st;
    if (c->tab == 1 && c->running) return 1;
    int y, m, d, hh, mm, ss;
    uihost_time(&y, &m, &d, &hh, &mm, &ss);
    if (ss != c->last_sec) { c->last_sec = ss; return 1; }
    return 0;
}

static void clock_init(uiapp_t *a, const char *arg) { (void)arg; ((clock_t_ *)a->st)->last_sec = -1; }

const uiapp_class_t app_clock = {
    "clock", "Clock", "clock", 360, 460, 0, sizeof(clock_t_),
    clock_init, clock_draw, clock_press, 0, 0, clock_key, 0, clock_tick, 0,
};
