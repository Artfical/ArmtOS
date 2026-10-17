#include "wm_notify.h"
#include <stdint.h>
#include "bcm.h"
#include "uart.h"
#include "irq.h"
#include "heap.h"
#include "fb.h"
#include "dwc2.h"
#include "usbhid.h"
#include "cfgfile.h"
#include "string_arm.h"
#include "compat/cmos.h"
#include "../../display/gfx.h"
#include "../../display/uiapp.h"
#include "../../display/uihost.h"
#include "../../display/imgload.h"

/*
 * The ArmtOS desktop: tOS's graphical desktop on the firmware's framebuffer, driven by a USB
 * mouse and keyboard. Every window is an app on the shared framework (uiapp.h) -- the same
 * Files, Notepad, Calculator, Settings... as tOS -- plus the Pi's own (apps_arm.c).
 *
 * There is no scheduler on the board, so the desktop runs whenever the shell waits for a key
 * (shell_readline calls desktop_tick), and the Terminal window is the shell itself: what the
 * shell prints is mirrored into it, and keys typed into it go to the shell.
 */

extern const uiapp_class_t app_terminal_arm, app_system, app_gpio, app_raspi;
static const uiapp_class_t *const arm_classes[] = { &app_terminal_arm, &app_system, &app_gpio, &app_raspi };
#define NARM 4
extern int term_changed;
void term_putc(char c);
void term_reset(void);
int netcmd_is_up(void);
extern uint32_t net_ip;

#define TOPBAR_H    28
#define TITLE_H     34
#define RADIUS      10
#define DOCK_ICON   48
#define DOCK_PAD    10
#define DOCK_GAP    10
#define DOCK_H      (DOCK_ICON + 2 * DOCK_PAD)
#define DOCK_MARGIN 8
#define MAXW        10

static int active;
int desktop_trace;                    /* "desktop trace": report the pointer on the serial line, for tests */
static int SW, SH;
static uint32_t *back, *wall, *wall_blur;
static gfx_surface_t bs;
static int mx, my, prev_btn;
static int cur_x = -100, cur_y = -100;
static int dirty;
static uint32_t last_frame_ms, last_term_ms;
static int last_minute = -1;
static int launcher_open, hover_dock = -1, hover_launch = -1, menu_open, hover_menu = -1;
static int wallpaper_choice, dock_magnify = 1, mouse_speed = 1;
static char custom_wallpaper[64];

extern void (*uart_mirror)(char c);

static uint32_t now_ms(void) { return (uint32_t)(timer_us() / 1000); }

static const uiapp_class_t *find_class(const char *id)
{
    for (int i = 0; i < NARM; i++) if (!strcmp(arm_classes[i]->id, id)) return arm_classes[i];
    return uiapp_find(id);
}

/* the launcher's list: the Pi's apps first, then the shared ones (Activity has nothing to show here) */
static const uiapp_class_t *launch_list[24];
static int nlaunch;

static void build_launch_list(void)
{
    nlaunch = 0;
    for (int i = 0; i < NARM; i++) launch_list[nlaunch++] = arm_classes[i];
    for (int i = 0; i < uiapp_class_count && nlaunch < 24; i++)
        if (strcmp(uiapp_classes[i]->id, "taskmgr")) launch_list[nlaunch++] = uiapp_classes[i];
}

/* ------------------------------------------------------------------ windows */

typedef struct { int open, minimized, x, y, w, h, z; uiapp_t *app; } dwin_t;
static dwin_t wins[MAXW];
static int next_z = 1, focused = -1;
static int drag = -1, drag_dx, drag_dy, grab = -1;

static void focus(int i) { wins[i].minimized = 0; wins[i].z = next_z++; focused = i; dirty = 1; }

void desktop_open(const char *id, const char *arg)
{
    if (!active) return;
    if (!strcmp(id, "run") || !strcmp(id, "pdf") || !strcmp(id, "music")) return;     /* nothing on the board does these */
    const uiapp_class_t *cls = find_class(id);
    if (!cls) return;
    /* one window per app here, except for documents */
    if (!arg)
        for (int i = 0; i < MAXW; i++) if (wins[i].open && wins[i].app->cls == cls) { focus(i); return; }
    int slot = -1;
    for (int i = 0; i < MAXW; i++) if (!wins[i].open) { slot = i; break; }
    if (slot < 0) return;
    uiapp_t *ap = malloc(sizeof(uiapp_t));
    void *st = malloc((size_t)cls->state_size + 1);
    if (!ap || !st) { if (ap) free(ap); if (st) free(st); return; }
    memset(ap, 0, sizeof(*ap));
    memset(st, 0, (size_t)cls->state_size + 1);
    ap->cls = cls; ap->st = st; ap->w = cls->w; ap->h = cls->h; ap->dirty = 1;
    strncpy(ap->title, cls->title, sizeof(ap->title) - 1);
    int n = 0;
    for (int i = 0; i < MAXW; i++) if (wins[i].open) n++;
    dwin_t *w = &wins[slot];
    w->open = 1; w->minimized = 0; w->app = ap;
    w->w = cls->w; w->h = TITLE_H + cls->h;
    w->x = (SW - w->w) / 2 - 60 + n * 30;
    w->y = TOPBAR_H + (SH - TOPBAR_H - DOCK_H - w->h) / 2 - 20 + n * 26;
    if (w->x < 6) w->x = 6;
    if (w->y < TOPBAR_H + 6) w->y = TOPBAR_H + 6;
    if (cls->init) cls->init(ap, arg);
    focus(slot);
}

static void close_win(int i)
{
    dwin_t *w = &wins[i];
    if (w->app->cls->close) w->app->cls->close(w->app);
    free(w->app->st);
    free(w->app);
    w->open = 0;
    if (focused == i) focused = -1;
    if (grab == i) grab = -1;
    dirty = 1;
}

static int window_at(int x, int y)
{
    int hit = -1;
    for (int i = 0; i < MAXW; i++) {
        dwin_t *w = &wins[i];
        if (!w->open || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) if (hit < 0 || w->z > wins[hit].z) hit = i;
    }
    return hit;
}

static int find_open(const char *id)
{
    for (int i = 0; i < MAXW; i++) if (wins[i].open && !strcmp(wins[i].app->cls->id, id)) return i;
    return -1;
}

/* ------------------------------------------------------------------ drawing */

static const gfx_image_t *icon(const char *key, int big)
{
    char name[32] = "icon48_";
    if (!big) { name[4] = '2'; name[5] = '0'; }
    strcpy(name + 7, key);
    return gfx_image(name);
}

static void glass(int x, int y, int w, int h, int r, uint32_t tint, int alpha)
{
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= SH) continue;
        int inset = 0;
        if (r > 0 && (j < r || j >= h - r)) {
            int dy = j < r ? r - j : j - (h - r) + 1, dx = 0;
            while (dx * dx + dy * dy < r * r && dx < r) dx++;
            inset = r - dx;
        }
        int x0 = x + inset, x1 = x + w - inset;
        if (x0 < 0) x0 = 0;
        if (x1 > SW) x1 = SW;
        if (x1 > x0) memcpy(back + py * SW + x0, wall_blur + py * SW + x0, (size_t)(x1 - x0) * 4);
    }
    gfx_rrect(&bs, x, y, w, h, r, tint, alpha);
}

static void draw_window(int i)
{
    dwin_t *w = &wins[i];
    uiapp_t *ap = w->app;
    int x = w->x, y = w->y, ww = w->w, hh = w->h, on = (i == focused);
    gfx_shadow(&bs, x, y, ww, hh, on ? 230 : 130);
    gfx_rrect(&bs, x, y, ww, hh, RADIUS, UI_BG, 255);
    gfx_rrect(&bs, x, y, ww, TITLE_H + RADIUS, RADIUS, on ? ui_theme.title_on : ui_theme.title_off, 255);
    gfx_hline(&bs, x + RADIUS, y, ww - 2 * RADIUS, 0xFFFFFF, 26);
    static const uint32_t col[3] = { 0xFF5F57, 0xFEBC2E, 0x28C840 };
    for (int k = 0; k < 3; k++) gfx_circle(&bs, x + 18 + k * 20, y + TITLE_H / 2, 6, on ? col[k] : (ui_theme.dark ? 0x4B4D57 : 0xD0D0D5), 255);
    int tw = gfx_text_width(&ui_font_medium, ap->title);
    int tx = x + (ww - tw) / 2;
    if (tx < x + 80) tx = x + 80;
    gfx_blit(&bs, icon(ap->cls->icon, 0), tx - 26, y + (TITLE_H - 20) / 2, 255);
    gfx_surface_t t = bs;
    gfx_clip(&t, x, y, ww - 8, TITLE_H);
    gfx_text(&t, &ui_font_medium, tx, y + 8, ap->title, on ? ui_theme.title_text : UI_DIM, 255);
    ap->w = ww; ap->h = hh - TITLE_H;
    gfx_surface_t c = bs;
    gfx_clip(&c, x, y + TITLE_H, ww, hh - TITLE_H - RADIUS / 2);
    gfx_fill(&c, x, y + TITLE_H, ww, hh - TITLE_H, UI_BG);
    ap->cls->draw(ap, &c, x, y + TITLE_H, ww, hh - TITLE_H);
    gfx_hline(&bs, x, y + TITLE_H, ww, 0, ui_theme.dark ? 90 : 30);
    gfx_rrect_outline(&bs, x, y, ww, hh, RADIUS, ui_theme.dark ? 0xFFFFFF : 0x000000, ui_theme.dark ? 30 : 40);
}

/* dock: the launcher, then these apps */
static const char *const dock_ids[] = { 0, "terminal", "files", "notepad", "gpio", "netmon", "settings", "raspi" };
#define NDOCK ((int)(sizeof(dock_ids) / sizeof(dock_ids[0])))
static int dock_x[NDOCK], dock_sz[NDOCK], dock_left, dock_w, dock_top;

static void layout_dock(void)
{
    int total = DOCK_PAD * 2;
    for (int i = 0; i < NDOCK; i++) {
        int d = hover_dock < 0 ? 99 : (i > hover_dock ? i - hover_dock : hover_dock - i);
        dock_sz[i] = !dock_magnify ? DOCK_ICON : d == 0 ? 60 : d == 1 ? 54 : DOCK_ICON;
        total += dock_sz[i] + (i ? DOCK_GAP : 0);
    }
    dock_w = total;
    dock_left = (SW - total) / 2;
    dock_top = SH - DOCK_MARGIN - DOCK_H;
    int x = dock_left + DOCK_PAD;
    for (int i = 0; i < NDOCK; i++) { dock_x[i] = x; x += dock_sz[i] + DOCK_GAP; }
}

static int dock_hit(int x, int y)
{
    if (y < dock_top - 14 || y >= dock_top + DOCK_H) return -1;
    for (int i = 0; i < NDOCK; i++) if (x >= dock_x[i] - DOCK_GAP / 2 && x < dock_x[i] + dock_sz[i] + DOCK_GAP / 2) return i;
    return -1;
}

static void draw_dock(void)
{
    layout_dock();
    glass(dock_left, dock_top, dock_w, DOCK_H, 18, 0xFFFFFF, 46);
    gfx_rrect_outline(&bs, dock_left, dock_top, dock_w, DOCK_H, 18, 0xFFFFFF, 70);
    for (int i = 0; i < NDOCK; i++) {
        const uiapp_class_t *c = dock_ids[i] ? find_class(dock_ids[i]) : 0;
        const char *key = c ? c->icon : "launcher";
        if (dock_ids[i] && !strcmp(dock_ids[i], "raspi")) key = "gpio";
        int sz = dock_sz[i];
        gfx_blit_scaled(&bs, icon(key, 1), dock_x[i], dock_top + DOCK_H - DOCK_PAD - sz, sz, sz, 255);
        if (dock_ids[i] && find_open(dock_ids[i]) >= 0) gfx_circle(&bs, dock_x[i] + sz / 2, dock_top + DOCK_H - 4, 2, 0xFFFFFF, 220);
    }
    if (hover_dock >= 0 && !menu_open) {
        const uiapp_class_t *c = dock_ids[hover_dock] ? find_class(dock_ids[hover_dock]) : 0;
        const char *label = c ? c->title : "Applications";
        int tw = gfx_text_width(&ui_font_regular, label);
        int lx = dock_x[hover_dock] + dock_sz[hover_dock] / 2 - (tw + 20) / 2, ly = dock_top - 40;
        gfx_shadow(&bs, lx, ly, tw + 20, 26, 120);
        gfx_rrect(&bs, lx, ly, tw + 20, 26, 8, ui_theme.menu, 240);
        gfx_text(&bs, &ui_font_regular, lx + 10, ly + 5, label, UI_TEXT, 255);
    }
}

static const char *const menu_labels[] = { "About This Pi", "System Settings...", "Applications", "Restart" };
#define NMENU 4
#define MITEM 26

static int menu_hit(int x, int y)
{
    if (!menu_open || x < 8 || x >= 8 + 210) return -1;
    int i = (y - (TOPBAR_H + 6)) / MITEM;
    return (y >= TOPBAR_H + 6 && i >= 0 && i < NMENU) ? i : -1;
}

static int eth_x = -100, cpu_x = -100, clock_x = 10000, search_x = -100;

static void draw_topbar(void)
{
    glass(0, 0, SW, TOPBAR_H, 0, ui_theme.bar_tint, ui_theme.bar_alpha);
    gfx_hline(&bs, 0, TOPBAR_H - 1, SW, 0, 70);
    uint32_t tc = ui_theme.bar_text;
    if (menu_open) gfx_rrect(&bs, 8, 3, 30, TOPBAR_H - 6, 6, ui_theme.dark ? 0xFFFFFF : 0x000000, 30);
    gfx_blit_tinted(&bs, gfx_image("logo"), 14, (TOPBAR_H - 18) / 2, tc, 255);
    const char *app = focused >= 0 ? wins[focused].app->cls->title : "ArmtOS";
    gfx_text(&bs, &ui_font_medium, 48, 6, app, tc, 255);
    gfx_text(&bs, &ui_font_regular, 48 + gfx_text_width(&ui_font_medium, app) + 22, 6, bcm_model_name(), UI_DIM, 255);
    cmos_time_t t;
    cmos_get_time(&t);
    char clk[8] = { (char)('0' + t.hour / 10), (char)('0' + t.hour % 10), ':', (char)('0' + t.minute / 10), (char)('0' + t.minute % 10), 0 };
    int cw = gfx_text_width(&ui_font_medium, clk);
    int rx = SW - 14 - cw;
    if (notify_center_is_open()) gfx_rrect(&bs, rx - 8, 3, cw + 16, TOPBAR_H - 6, 6, ui_theme.dark ? 0xFFFFFF : 0x000000, 30);
    gfx_text(&bs, &ui_font_medium, rx, 6, clk, tc, 255);
    clock_x = rx - 8;
    rx -= 30;
    if (notify_dnd()) { gfx_blit_tinted(&bs, gfx_image("glyph_moon"), rx, 6, tc, 230); rx -= 28; }
    search_x = rx;
    gfx_blit_tinted(&bs, gfx_image("glyph_search"), rx, 6, tc, 230);
    rx -= 28;
    gfx_blit_tinted(&bs, gfx_image("glyph_ethernet-port"), rx, 6, tc, netcmd_is_up() && net_ip ? 230 : 90);
    eth_x = rx;
    rx -= 28;
    gfx_blit_tinted(&bs, gfx_image("glyph_cpu"), rx, 6, tc, 200);
    cpu_x = rx;
    if (menu_open) {
        int h = NMENU * MITEM + 10;
        gfx_shadow(&bs, 8, TOPBAR_H + 2, 210, h, 200);
        gfx_rrect(&bs, 8, TOPBAR_H + 2, 210, h, 9, ui_theme.menu, 245);
        for (int i = 0; i < NMENU; i++) {
            int iy = TOPBAR_H + 6 + i * MITEM;
            if (i == hover_menu) gfx_rrect(&bs, 13, iy, 200, MITEM, 6, UI_ACCENT, 255);
            gfx_text(&bs, &ui_font_regular, 24, iy + 5, menu_labels[i], i == hover_menu ? 0xFFFFFF : UI_TEXT, 255);
        }
    }
}

#define L_COLS 6
#define L_CW 130
#define L_CH 112

static void launcher_origin(int *ox, int *oy)
{
    int rows = (nlaunch + L_COLS - 1) / L_COLS;
    *ox = (SW - L_COLS * L_CW) / 2;
    *oy = (SH - rows * L_CH) / 2 + 10;
}

static int launcher_hit(int x, int y)
{
    int ox, oy;
    launcher_origin(&ox, &oy);
    if (x < ox || y < oy) return -1;
    int c = (x - ox) / L_CW, r = (y - oy) / L_CH;
    if (c >= L_COLS) return -1;
    int i = r * L_COLS + c;
    return i < nlaunch ? i : -1;
}

static void draw_launcher(void)
{
    memcpy(back, wall_blur, (size_t)SW * SH * 4);
    gfx_fill_alpha(&bs, 0, 0, SW, SH, 0, 90);
    int ox, oy;
    launcher_origin(&ox, &oy);
    gfx_text_center(&bs, &ui_font_title, 0, oy - 70, SW, "Applications", 0xF2F3F5, 255);
    for (int i = 0; i < nlaunch; i++) {
        int cx = ox + (i % L_COLS) * L_CW, cy = oy + (i / L_COLS) * L_CH;
        if (i == hover_launch) gfx_rrect(&bs, cx + 10, cy, L_CW - 20, L_CH - 10, 14, 0xFFFFFF, 40);
        const char *key = !strcmp(launch_list[i]->id, "raspi") ? "gpio" : launch_list[i]->icon;
        gfx_blit_scaled(&bs, icon(key, 1), cx + (L_CW - 64) / 2, cy + 10, 64, 64, 255);
        gfx_text_center(&bs, &ui_font_regular, cx, cy + 80, L_CW, launch_list[i]->title, 0xF2F3F5, 255);
    }
}

static void compose(void)
{
    if (launcher_open) { draw_launcher(); return; }
    memcpy(back, wall, (size_t)SW * SH * 4);
    int order[MAXW], n = 0;
    for (int i = 0; i < MAXW; i++) if (wins[i].open && !wins[i].minimized) order[n++] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && wins[order[j]].z > wins[k].z) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    for (int i = 0; i < n; i++) draw_window(order[i]);
    draw_dock();
    draw_topbar();
    notify_draw(&bs, SW, SH, TOPBAR_H);
}

/* ------------------------------------------------------------------ the framebuffer */

static inline uint32_t to_fb(uint32_t c)
{
    return fb_swapped() ? ((c & 0x00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF)) : c;
}

static void copy_out(int x0, int y0, int w, int h)
{
    volatile uint32_t *fb = fb_pixels();
    uint32_t pitch = fb_pitch();
    int sw = fb_swapped();
    for (int j = 0; j < h; j++) {
        int y = y0 + j;
        if (y < 0 || y >= SH) continue;
        const uint32_t *src = back + y * SW;
        volatile uint32_t *dst = fb + (uint32_t)y * pitch;
        for (int i = 0; i < w; i++) {
            int x = x0 + i;
            if (x < 0 || x >= SW) continue;
            uint32_t c = src[x];
            dst[x] = sw ? ((c & 0x00FF00) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF)) : c;
        }
    }
}

static void cursor_draw(void)
{
    const gfx_image_t *c = gfx_image("cursor");
    if (!c) return;
    volatile uint32_t *fb = fb_pixels();
    uint32_t pitch = fb_pitch();
    int ox = mx - 1, oy = my - 1;
    for (int j = 0; j < c->h; j++) {
        int y = oy + j;
        if (y < 0 || y >= SH) continue;
        for (int i = 0; i < c->w; i++) {
            int x = ox + i;
            if (x < 0 || x >= SW) continue;
            const uint8_t *p = c->rgba + (j * c->w + i) * 4;
            if (p[3]) fb[(uint32_t)y * pitch + x] = to_fb(gfx_mix(back[y * SW + x], gfx_rgb(p[0], p[1], p[2]), p[3]));
        }
    }
    cur_x = ox; cur_y = oy;
}

static void present(void) { copy_out(0, 0, SW, SH); cursor_draw(); }

/* while a long command prints, the desktop is not ticking: the Terminal is repainted from here */
static void mirror(char c)
{
    term_putc(c);
    if (!active) return;
    dirty = 1;
    uint32_t t = now_ms();
    if (t - last_term_ms > 120) {
        last_term_ms = t;
        compose();
        present();
        dirty = 0;
    }
}

/* ------------------------------------------------------------------ wallpaper, mode, settings */

static void set_wallpaper(int c)
{
    if (c == 3 && custom_wallpaper[0]) {
        uint8_t *rgba; uint32_t w, h;
        const char *err;
        if (img_load(custom_wallpaper, &rgba, &w, &h, &err) == 0) {
            gfx_wallpaper_from(wall, SW, SH, wall_blur, rgba, (int)w, (int)h, 4);
            free(rgba);
            wallpaper_choice = 3;
            dirty = 1;
            return;
        }
    }
    if (c == 3) c = 0;
    wallpaper_choice = c;
    if (c == 0) gfx_wallpaper(wall, SW, SH, wall_blur);
    else {
        gfx_surface_t ws;
        gfx_surface_init(&ws, wall, SW, SH);
        gfx_gradient_v(&ws, 0, 0, SW, SH, c == 1 ? 0x0B3A66 : 0x2A2C33, c == 1 ? 0x2BB3C9 : 0x5A5F6B);
        memcpy(wall_blur, wall, (size_t)SW * SH * 4);
    }
    dirty = 1;
}

int desktop_set_wallpaper_file(const char *path)
{
    strncpy(custom_wallpaper, path, sizeof(custom_wallpaper) - 1);
    set_wallpaper(3);
    return wallpaper_choice == 3 ? 0 : -1;
}

static int alloc_buffers(int w, int h)
{
    size_t n = (size_t)w * h * 4;
    if (back) free(back);
    if (wall) free(wall);
    if (wall_blur) free(wall_blur);
    back = malloc(n); wall = malloc(n); wall_blur = malloc(n);
    if (!back || !wall || !wall_blur) return -1;
    gfx_surface_init(&bs, back, w, h);
    return 0;
}

static int set_resolution(int w, int h)
{
    if (w == SW && h == SH) return 0;
    if (fb_init((uint32_t)w, (uint32_t)h) != 0) return -1;
    SW = (int)fb_width(); SH = (int)fb_height();
    if (alloc_buffers(SW, SH) != 0) return -1;
    set_wallpaper(wallpaper_choice);
    usbhid_set_screen(SW, SH);
    mx = SW / 2; my = SH / 2;
    for (int i = 0; i < MAXW; i++) {
        if (wins[i].x + wins[i].w > SW) wins[i].x = SW - wins[i].w > 0 ? SW - wins[i].w : 0;
        if (wins[i].y + wins[i].h > SH - DOCK_H) wins[i].y = TOPBAR_H + 6;
    }
    dirty = 1;
    return 0;
}

int desktop_get(const char *key)
{
    if (!strcmp(key, "dark")) return ui_theme.dark;
    if (!strcmp(key, "dock_magnify")) return dock_magnify;
    if (!strcmp(key, "mouse_speed")) return mouse_speed;
    if (!strcmp(key, "wallpaper")) return wallpaper_choice;
    if (!strcmp(key, "width")) return SW;
    if (!strcmp(key, "height")) return SH;
    if (!strcmp(key, "dnd")) return notify_dnd();
    return -1;
}

void desktop_set(const char *key, int v)
{
    if (!strcmp(key, "dark")) ui_set_dark(v);
    else if (!strcmp(key, "dock_magnify")) dock_magnify = v;
    else if (!strcmp(key, "mouse_speed")) mouse_speed = v;
    else if (!strcmp(key, "wallpaper")) set_wallpaper(v);
    else if (!strcmp(key, "dnd")) notify_set_dnd(v);
    else if (!strcmp(key, "resolution")) {
        static const int rw[5] = { 1024, 1280, 1366, 1600, 1920 }, rh[5] = { 768, 720, 768, 900, 1080 };
        if (v >= 0 && v < 5) set_resolution(rw[v], rh[v]);
    }
    dirty = 1;
}

/* ------------------------------------------------------------------ input */

static void restart(void)
{
    uart_puts("restarting\n");
    mmio_write(bcm_periph_base + 0x10001c, 0x5A000020 | 1);
    mmio_write(bcm_periph_base + 0x100024, 0x5A000000 | (1 << 5));
    for (;;) __asm__ volatile("wfe");
}

static void press(int x, int y)
{
    /* notifications first: banners, and Notification Centre while it is open */
    {
        char app[24];
        int was_open = notify_center_is_open();
        if (notify_press(x, y, app, sizeof(app))) { dirty = 1; if (app[0]) desktop_open(app, 0); return; }
        if (was_open) dirty = 1;
        if (y < TOPBAR_H && x >= clock_x) { if (!was_open) notify_center_toggle(); menu_open = launcher_open = 0; dirty = 1; return; }
    }
    if (menu_open) {
        int i = menu_hit(x, y);
        menu_open = 0;
        dirty = 1;
        if (i == 0) desktop_open("system", 0);
        else if (i == 1) desktop_open("settings", 0);
        else if (i == 2) launcher_open = 1;
        else if (i == 3) restart();
        if (i >= 0 || x < 40) return;
    }
    if (launcher_open) {
        int i = launcher_hit(x, y);
        launcher_open = 0;
        dirty = 1;
        if (i >= 0) desktop_open(launch_list[i]->id, 0);
        return;
    }
    if (y < TOPBAR_H) {
        if (x < 40) { menu_open = 1; hover_menu = -1; dirty = 1; }
        else if (x >= eth_x - 6 && x < eth_x + 22) desktop_open("netmon", 0);
        else if (x >= cpu_x - 6 && x < cpu_x + 22) desktop_open("system", 0);
        else if (x >= search_x - 6 && x < search_x + 22) { launcher_open = 1; dirty = 1; }
        return;
    }
    int d = dock_hit(x, y);
    if (d >= 0) {
        if (!dock_ids[d]) { launcher_open = 1; dirty = 1; return; }
        int i = find_open(dock_ids[d]);
        if (i >= 0 && focused == i && !wins[i].minimized) { wins[i].minimized = 1; dirty = 1; }
        else if (i >= 0) focus(i);
        else desktop_open(dock_ids[d], 0);
        return;
    }
    int i = window_at(x, y);
    if (i < 0) return;
    focus(i);
    dwin_t *w = &wins[i];
    int lx = x - w->x, ly = y - w->y;
    if (ly < TITLE_H) {
        for (int k = 0; k < 3; k++) {
            int cx = 18 + k * 20, cy = TITLE_H / 2;
            if ((lx - cx) * (lx - cx) + (ly - cy) * (ly - cy) <= 64) {
                if (k == 0) close_win(i); else w->minimized = 1;
                dirty = 1;
                return;
            }
        }
        drag = i; drag_dx = lx; drag_dy = ly;
        return;
    }
    grab = i;
    if (w->app->cls->press) w->app->cls->press(w->app, lx, ly - TITLE_H, 1);
    dirty = 1;
}

void desktop_tick(void)
{
    if (!active) return;
    usbhid_poll();
    int nx, ny, btn;
    usbhid_pointer(&nx, &ny, &btn);
    int moved = nx != mx || ny != my;
    mx = nx; my = ny;
    if (moved && desktop_trace) {
        char b[32] = "P ";
        ui_num(b, mx); strcat(b, " "); ui_num(b, my); strcat(b, "\n");
        void (*m)(char) = uart_mirror;
        uart_mirror = 0;
        uart_puts(b);
        uart_mirror = m;
    }
    int pressed = (btn & 1) && !(prev_btn & 1);
    prev_btn = btn;

    if (drag >= 0) {
        if (btn & 1) {
            wins[drag].x = mx - drag_dx;
            wins[drag].y = my - drag_dy;
            if (wins[drag].y < TOPBAR_H) wins[drag].y = TOPBAR_H;
            if (moved) dirty = 1;
        } else drag = -1;
    } else if (grab >= 0) {
        dwin_t *w = &wins[grab];
        if (!w->open) grab = -1;
        else if (btn & 1) { if (moved && w->app->cls->drag) { w->app->cls->drag(w->app, mx - w->x, my - w->y - TITLE_H); } }
        else { if (w->app->cls->release) w->app->cls->release(w->app, mx - w->x, my - w->y - TITLE_H); grab = -1; }
    } else if (pressed) press(mx, my);

    int wheel = usbhid_wheel();
    if (wheel) {
        int i = window_at(mx, my);
        if (i >= 0 && wins[i].app->cls->wheel) wins[i].app->cls->wheel(wins[i].app, wheel);
    }

    if (moved) {
        layout_dock();
        int hd = (!menu_open && !launcher_open && drag < 0) ? dock_hit(mx, my) : -1;
        if (hd != hover_dock) { hover_dock = hd; dirty = 1; }
        int hm = menu_hit(mx, my);
        if (hm != hover_menu) { hover_menu = hm; dirty = 1; }
        int hl = launcher_open ? launcher_hit(mx, my) : -1;
        if (hl != hover_launch) { hover_launch = hl; dirty = 1; }
    }
    for (int i = 0; i < MAXW; i++) {
        if (!wins[i].open) continue;
        uiapp_t *ap = wins[i].app;
        if (!wins[i].minimized && ap->cls->tick && ap->cls->tick(ap)) dirty = 1;
        if (ap->dirty) { ap->dirty = 0; dirty = 1; }
    }
    if (notify_tick()) dirty = 1;
    cmos_time_t t;
    cmos_get_time(&t);
    if (t.minute != last_minute) { last_minute = t.minute; dirty = 1; }

    uint32_t now = now_ms();
    if (dirty && now - last_frame_ms >= 30) {
        last_frame_ms = now;
        last_term_ms = now;
        dirty = 0;
        compose();
        present();
    } else if (moved) {
        copy_out(cur_x, cur_y, 26, 28);
        cursor_draw();
    }
}

/* Keys from the USB keyboard: to the shell when the Terminal has the focus, to the app otherwise. */
int desktop_getc(void)
{
    if (!active) return -1;
    int c = usbhid_getc();
    if (c < 0) return -1;
    if (focused >= 0 && wins[focused].open && strcmp(wins[focused].app->cls->id, "terminal")) {
        uiapp_t *ap = wins[focused].app;
        if (ap->cls->key) ap->cls->key(ap, c);
        dirty = 1;
        return -1;
    }
    if (c >= UI_KEY_UP && c <= UI_KEY_RIGHT) return -1;
    return c;
}

int desktop_active(void) { return active; }

int desktop_start(int w, int h)
{
    if (active) return 0;
    if (gfx_init() != 0) { uart_puts("desktop: could not unpack its assets\n"); return -1; }
    uint32_t cw = 0, chh = 0;
    if (!w && bootcfg_framebuffer(&cw, &chh)) { w = (int)cw; h = (int)chh; }
    if (!w) { w = 1280; h = 720; }
    if (fb_init((uint32_t)w, (uint32_t)h) != 0) { uart_puts("desktop: the firmware gave no framebuffer\n"); return -1; }
    SW = (int)fb_width(); SH = (int)fb_height();
    if (alloc_buffers(SW, SH) != 0) { uart_puts("desktop: not enough memory\n"); return -1; }
    set_wallpaper(0);
    build_launch_list();
    if (usb_ensure() == 0) usbhid_init();
    usbhid_set_screen(SW, SH);
    mx = SW / 2; my = SH / 2;
    term_reset();
    uart_mirror = mirror;
    active = 1;
    desktop_open("terminal", 0);
    compose();
    present();
    char m[64] = "desktop: ";
    ui_num(m, SW); strcat(m, "x"); ui_num(m, SH);
    uart_puts(m);
    uart_puts(usbhid_has_mouse() ? ", USB mouse" : ", no mouse");
    uart_puts(usbhid_has_keyboard() ? ", USB keyboard\n" : ", no keyboard (type on the serial console)\n");
    return 0;
}

/* ------------------------------------------------------------------ notifications for apps */

int desktop_notify(const char *app, const char *icon, const char *title, const char *body, const char *act1, const char *act2)
{
    int id = notify_post(app, icon, title, body, act1, act2);
    dirty = 1;
    return id;
}
