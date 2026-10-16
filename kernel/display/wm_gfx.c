#include "wm.h"
#include "wm_internal.h"
#include "gfx.h"
#include "bochs.h"
#include "mouse.h"
#include "scheduler.h"
#include "string.h"
#include "memory.h"
#include "paging.h"
#include "cmos.h"
#include "net.h"
#include "sound.h"
#include "fsbridge.h"
#include "debugmon.h"
#include "io.h"

/*
 * The graphical desktop.
 *
 * A pixel framebuffer (Bochs/QEMU/VirtualBox VBE) at 1280x720 by default, composed in a back
 * buffer and copied out whole: wallpaper, windows with soft shadows and rounded corners, a
 * frosted top bar and dock, menus, an app launcher, and built-in Settings and About panels.
 *
 * Every existing app keeps working unchanged: they draw into an 80x23 character surface
 * (terminal.h), and a window here shows that surface with an anti-aliased monospace font.
 * Only the frame around it -- and the panels -- are drawn in pixels.
 *
 * The window list, focus, app start-up and the app-facing calls stay in wm.c; this file only
 * draws and turns the mouse into those same actions.
 */

#define TOPBAR_H    28
#define TITLE_H     34
#define RADIUS      10
#define DOCK_ICON   48
#define DOCK_PAD    10
#define DOCK_GAP    10
#define DOCK_H      (DOCK_ICON + 2 * DOCK_PAD)
#define DOCK_MARGIN 8
#define CW          UI_CELL_W
#define CH          UI_CELL_H
#define CPAD_X      10      /* space between the frame and the character grid */
#define CPAD_Y      6

/* colours */
#define C_TEXT       0xF2F3F5
#define C_TEXT_DIM   0xA1A4AE
#define C_ACCENT     0x3B82F6
#define C_TITLE_ON   0x2C2E37
#define C_TITLE_OFF  0x24252C
#define C_MENU_BG    0x26272E

/* the 16 VGA colours of the character surfaces, redrawn as a modern palette */
static const uint32_t pal[16] = {
    0x1A1B21, 0x3B82F6, 0x22C55E, 0x06B6D4, 0xEF4444, 0xD946EF, 0xF59E0B, 0xD4D4D8,
    0x71717A, 0x60A5FA, 0x4ADE80, 0x22D3EE, 0xF87171, 0xF0ABFC, 0xFDE047, 0xFAFAFA,
};

static bochs_device_t vdev;
static int SW, SH;
static uint32_t *lfb;
static uint32_t *back, *wall, *wall_blur, *scratch;
static gfx_surface_t bs;

static int mx, my;                    /* pointer, pixels */
static uint8_t prev_btn;
static int cur_x = -100, cur_y = -100;    /* where the cursor is drawn on screen */
static int dirty = 1;
static uint32_t last_frame_ms;
static int last_minute = -1;
static int ext_graphics;              /* another program owned the display last frame */

static window_t *drag_win, *resize_win;
static int drag_dx, drag_dy;
static uint32_t last_title_click_ms;
static window_t *last_title_click_win;

static int launcher_open;
static int hover_dock = -1, hover_launch = -1, hover_menu = -1, hover_lights;
static int dock_magnify = 1;
static int wallpaper_choice;          /* 0 dusk (picture), 1 ocean, 2 graphite */
static int mouse_speed = 1;

/* ------------------------------------------------------------------ apps */

typedef struct {
    const char *id, *title, *icon;
    int kind, dock, panel;
} app_t;

enum { PANEL_SETTINGS = 1, PANEL_ABOUT = 2 };

static const app_t apps[] = {
    { "files",       "Files",         "files",      WIN_KIND_FILEMGR,     1, 0 },
    { "terminal",    "Terminal",      "terminal",   WIN_KIND_TERMINAL,    1, 0 },
    { "notepad",     "Notepad",       "notepad",    WIN_KIND_NOTEPAD,     1, 0 },
    { "calculator",  "Calculator",    "calculator", WIN_KIND_CALCULATOR,  1, 0 },
    { "paint",       "Paint",         "paint",      WIN_KIND_PAINT,       1, 0 },
    { "mediaplayer", "Music",         "media",      WIN_KIND_MEDIAPLAYER, 1, 0 },
    { "netmon",      "Network",       "netmon",     WIN_KIND_NETMON,      1, 0 },
    { "taskmgr",     "Activity",      "taskmgr",    WIN_KIND_TASKMGR,     1, 0 },
    { "@settings",   "Settings",      "settings",   WIN_KIND_PANEL,       1, PANEL_SETTINGS },
    { "viewer",      "Image Viewer",  "viewer",     WIN_KIND_VIEWER,      0, 0 },
    { "pdf",         "PDF Viewer",    "pdf",        WIN_KIND_PDFVIEWER,   0, 0 },
    { "notes",       "Sticky Notes",  "sticky",     WIN_KIND_STICKYNOTES, 0, 0 },
    { "clock",       "Clock",         "clock",      WIN_KIND_CLOCK,       0, 0 },
    { "diskutil",    "Disk Utility",  "diskmgr",    WIN_KIND_DISKMGR,     0, 0 },
    { "snake",       "Snake",         "snake",      WIN_KIND_SNAKE,       0, 0 },
    { "2048",        "2048",          "2048",       WIN_KIND_2048,        0, 0 },
    { "doom",        "DOOM",          "doom",       WIN_KIND_DOOM,        0, 0 },
    { "@about",      "About tOS",     "about",      WIN_KIND_PANEL,       0, PANEL_ABOUT },
};
#define NAPPS ((int)(sizeof(apps) / sizeof(apps[0])))

static const app_t *app_for_window(const window_t *w)
{
    for (int i = 0; i < NAPPS; i++) {
        if (apps[i].kind != w->kind) continue;
        if (w->kind == WIN_KIND_PANEL && apps[i].panel != w->panel) continue;
        return &apps[i];
    }
    return 0;
}

static const gfx_image_t *icon(const char *key, int big)
{
    char name[32] = "icon48_";
    if (!big) { name[4] = '2'; name[5] = '0'; }
    strcpy(name + 7, key);
    return gfx_image(name);
}

static void open_panel(int panel, const char *title)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows[i].open && windows[i].kind == WIN_KIND_PANEL && windows[i].panel == panel) {
            wm_focus_window(&windows[i]);
            return;
        }
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1; w->minimized = 0; w->maximized = 0;
    w->kind = WIN_KIND_PANEL;
    w->panel = panel;
    w->pid = -1;
    w->initial_cmd[0] = 0;
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = 0;
    wm_gfx_window_opened(w);
    wm_focus_window(w);
    sound_open();
}

static void launch(const app_t *a)
{
    sound_click();
    if (a->kind == WIN_KIND_PANEL) open_panel(a->panel, a->title);
    else wm_open_app(a->id);
    dirty = 1;
}

/* dock click: bring an open window of that app forward, or start one */
static void dock_activate(const app_t *a)
{
    window_t *best = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->open || app_for_window(w) != a) continue;
        if (!best || w->z > best->z) best = w;
    }
    if (!best) { launch(a); return; }
    sound_click();
    if (best == wm_focused && !best->minimized) best->minimized = 1;
    else wm_focus_window(best);
    dirty = 1;
}

/* ------------------------------------------------------------------ geometry */

static void set_cells(window_t *w, int cols, int rows)
{
    if (cols < 20) cols = 20;
    if (cols > TERM_SURFACE_W) cols = TERM_SURFACE_W;
    if (rows < 5) rows = 5;
    if (rows > TERM_SURFACE_H) rows = TERM_SURFACE_H;
    w->w0 = cols;
    w->h0 = rows + 1;
    w->gw = cols * CW + 2 * CPAD_X;
    w->gh = TITLE_H + rows * CH + 2 * CPAD_Y;
}

static void panel_size(int panel, int *pw, int *ph)
{
    if (panel == PANEL_SETTINGS) { *pw = 620; *ph = 470; }
    else { *pw = 440; *ph = 380; }
}

static void clamp_window(window_t *w)
{
    if (w->gx < -w->gw + 80) w->gx = -w->gw + 80;
    if (w->gx > SW - 80) w->gx = SW - 80;
    if (w->gy < TOPBAR_H) w->gy = TOPBAR_H;
    if (w->gy > SH - TITLE_H - DOCK_MARGIN) w->gy = SH - TITLE_H - DOCK_MARGIN;
}

void wm_gfx_window_opened(window_t *w)
{
    if (w->kind == WIN_KIND_PANEL) {
        panel_size(w->panel, &w->gw, &w->gh);
        w->w0 = 1; w->h0 = 1;
    } else {
        int big = w->kind == WIN_KIND_TERMINAL || w->kind == WIN_KIND_NOTEPAD || w->kind == WIN_KIND_FILEMGR ||
                  w->kind == WIN_KIND_TASKMGR || w->kind == WIN_KIND_NETMON || w->kind == WIN_KIND_MEDIAPLAYER ||
                  w->kind == WIN_KIND_PAINT || w->kind == WIN_KIND_VIEWER || w->kind == WIN_KIND_PDFVIEWER ||
                  w->kind == WIN_KIND_SCRIPT;
        if (w->maximized) set_cells(w, TERM_SURFACE_W, TERM_SURFACE_H);
        else if (big) set_cells(w, 80, 22);
        else set_cells(w, 60, 20);
    }
    int open_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].open && &windows[i] != w) open_count++;
    w->gx = (SW - w->gw) / 2 - 70 + open_count * 30;
    w->gy = TOPBAR_H + (SH - TOPBAR_H - DOCK_H - DOCK_MARGIN - w->gh) / 2 - 30 + open_count * 28;
    clamp_window(w);
    w->g_rest_x = w->gx; w->g_rest_y = w->gy; w->g_rest_w = w->w0; w->g_rest_h = w->h0;
    w->g_sum = 0xFFFFFFFF;
    w->anim = 0;                     /* the fade-in is off: it left windows half-drawn */
    dirty = 1;
}

static void zoom_window(window_t *w)
{
    if (w->kind == WIN_KIND_PANEL) return;
    if (!w->maximized) {
        w->g_rest_x = w->gx; w->g_rest_y = w->gy; w->g_rest_w = w->w0; w->g_rest_h = w->h0;
        set_cells(w, TERM_SURFACE_W, TERM_SURFACE_H);
        w->gx = (SW - w->gw) / 2;
        w->gy = TOPBAR_H + 10;
        if (w->gy + w->gh > SH - DOCK_H - DOCK_MARGIN) w->gy = TOPBAR_H;
        w->maximized = 1;
    } else {
        set_cells(w, w->g_rest_w, w->g_rest_h - 1);
        w->gx = w->g_rest_x; w->gy = w->g_rest_y;
        w->maximized = 0;
    }
    clamp_window(w);
    dirty = 1;
}

int wm_gfx_content_mouse(window_t *w, int *x, int *y)
{
    int lx = mx - w->gx - CPAD_X, ly = my - (w->gy + TITLE_H + CPAD_Y);
    if (x) *x = lx >= 0 ? lx / CW : -1 - (-lx - 1) / CW;
    if (y) *y = ly >= 0 ? ly / CH : -1 - (-ly - 1) / CH;
    return 1;
}

/* ------------------------------------------------------------------ menus */

enum {
    A_NONE, A_ABOUT, A_SETTINGS, A_TERMINAL, A_ACTIVITY, A_LAUNCHER, A_SHUTDOWN,
    A_NEW_TERM, A_NEW, A_OPEN, A_SAVE, A_CLOSE, A_MINIMIZE, A_ZOOM, A_FOCUS0,
    A_NEW_FOLDER = 40, A_NEW_FILE, A_FILES, A_WALLPAPER,
};

typedef struct { const char *label; int action; int sep_after; } menu_item_t;

static menu_item_t menu_items[16];
static int menu_count, menu_x, menu_y, menu_w, menu_which;   /* 0 = closed */
enum { MENU_SYS = 1, MENU_FILE_, MENU_WINDOW, MENU_CTX };
static int menu_label_x[4], menu_label_w[4];

static void menu_add(const char *label, int action, int sep)
{
    if (menu_count >= 16) return;
    menu_items[menu_count].label = label;
    menu_items[menu_count].action = action;
    menu_items[menu_count].sep_after = sep;
    menu_count++;
}

#define ITEM_H 26

static int menu_height(void)
{
    int h = 8;
    for (int i = 0; i < menu_count; i++) h += ITEM_H + (menu_items[i].sep_after ? 9 : 0);
    return h;
}

static void open_menu(int which, int x, int y)
{
    menu_count = 0;
    menu_which = which;
    if (which == MENU_SYS) {
        menu_add("About tOS", A_ABOUT, 0);
        menu_add("Settings...", A_SETTINGS, 1);
        menu_add("Applications", A_LAUNCHER, 0);
        menu_add("Terminal", A_TERMINAL, 0);
        menu_add("Activity Monitor", A_ACTIVITY, 1);
        menu_add("Shut Down...", A_SHUTDOWN, 0);
    } else if (which == MENU_FILE_) {
        if (wm_focused && wm_focused->kind == WIN_KIND_NOTEPAD) {
            menu_add("New", A_NEW, 0);
            menu_add("Open...", A_OPEN, 0);
            menu_add("Save", A_SAVE, 1);
        }
        menu_add("New Terminal Window", A_NEW_TERM, 1);
        menu_add("Close Window", A_CLOSE, 0);
    } else if (which == MENU_WINDOW) {
        menu_add("Minimize", A_MINIMIZE, 0);
        menu_add("Zoom", A_ZOOM, 1);
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (windows[i].open) menu_add(windows[i].title, A_FOCUS0 + i, 0);
    } else {
        menu_add("New Folder", A_NEW_FOLDER, 0);
        menu_add("New File", A_NEW_FILE, 1);
        menu_add("Open Files", A_FILES, 0);
        menu_add("Open Terminal", A_TERMINAL, 1);
        menu_add("Change Wallpaper", A_WALLPAPER, 0);
        menu_add("Settings...", A_SETTINGS, 0);
    }
    menu_w = 180;
    for (int i = 0; i < menu_count; i++) {
        int tw = gfx_text_width(&ui_font_regular, menu_items[i].label) + 40;
        if (tw > menu_w) menu_w = tw;
    }
    menu_x = x; menu_y = y;
    if (menu_x + menu_w > SW - 6) menu_x = SW - 6 - menu_w;
    if (menu_y + menu_height() > SH - 6) menu_y = SH - 6 - menu_height();
    if (menu_x < 4) menu_x = 4;
    hover_menu = -1;
    dirty = 1;
}

static int menu_hit(int x, int y)
{
    if (!menu_which || x < menu_x || x >= menu_x + menu_w) return -1;
    int iy = menu_y + 4;
    for (int i = 0; i < menu_count; i++) {
        if (y >= iy && y < iy + ITEM_H) return i;
        iy += ITEM_H + (menu_items[i].sep_after ? 9 : 0);
    }
    return -1;
}

static void shutdown_screen(void);
static void set_wallpaper(int choice);

static void do_action(int a)
{
    window_t *f = wm_focused;
    switch (a) {
    case A_ABOUT:     open_panel(PANEL_ABOUT, "About tOS"); break;
    case A_SETTINGS:  open_panel(PANEL_SETTINGS, "Settings"); break;
    case A_TERMINAL:
    case A_NEW_TERM:  wm_open_app("terminal"); break;
    case A_ACTIVITY:  wm_open_app("taskmgr"); break;
    case A_LAUNCHER:  launcher_open = 1; break;
    case A_SHUTDOWN:  shutdown_screen(); break;
    case A_NEW:       if (f) { action_target = f; pending_action = WM_ACTION_NEW; } break;
    case A_OPEN:      if (f) { action_target = f; pending_action = WM_ACTION_OPEN; } break;
    case A_SAVE:      if (f) { action_target = f; pending_action = WM_ACTION_SAVE; } break;
    case A_CLOSE:     if (f) wm_close_window(f); break;
    case A_MINIMIZE:  if (f) f->minimized = 1; break;
    case A_ZOOM:      if (f) zoom_window(f); break;
    case A_NEW_FOLDER: wm_desktop_new_folder(); break;
    case A_NEW_FILE:  wm_desktop_new_file(); break;
    case A_FILES:     wm_open_app("files"); break;
    case A_WALLPAPER: set_wallpaper((wallpaper_choice + 1) % 3); break;
    default:
        if (a >= A_FOCUS0 && a < A_FOCUS0 + MAX_WINDOWS && windows[a - A_FOCUS0].open)
            wm_focus_window(&windows[a - A_FOCUS0]);
        break;
    }
    dirty = 1;
}

/* ------------------------------------------------------------------ drawing */

static void glass(int x, int y, int w, int h, int r, uint32_t tint, int tint_alpha)
{
    /* the frosted wallpaper behind, then a tint; the rounded corners come from drawing the
     * blurred copy row by row inside the rounded shape */
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= SH) continue;
        int inset = 0;
        if (r > 0 && (j < r || j >= h - r)) {
            int dy = j < r ? r - j : j - (h - r) + 1;
            int dx = 0;
            while (dx * dx + dy * dy < r * r && dx < r) dx++;
            inset = r - dx;
        }
        int x0 = x + inset, x1 = x + w - inset;
        if (x0 < 0) x0 = 0;
        if (x1 > SW) x1 = SW;
        if (x1 > x0) memcpy(back + py * SW + x0, wall_blur + py * SW + x0, (size_t)(x1 - x0) * 4);
    }
    gfx_rrect(&bs, x, y, w, h, r, tint, tint_alpha);
}

static void draw_wallpaper(void)
{
    memcpy(back, wall, (size_t)SW * SH * 4);
}

static uint32_t surface_sum(window_t *w)
{
    uint32_t s = (uint32_t)w->surface.view_offset * 2654435761u ^ (uint32_t)w->surface.cur_row;
    const uint16_t *c = w->surface.cells;
    for (int i = 0; i < TERM_SURFACE_W * TERM_SURFACE_H; i++) s = (s << 5) + s + c[i];
    return s;
}

static void draw_cells(window_t *w, int x0, int y0)
{
    int cols = w->w0, rows = w->h0 - 1;
    int vo = w->surface.view_offset;
    int newest = w->surface.cur_row;
    if (newest >= TERM_SURFACE_H) newest = TERM_SURFACE_H - 1;
    int overflow = newest - (rows - 1);
    if (overflow < 0) overflow = 0;
    for (int row = 0; row < rows; row++) {
        int idx = overflow + row - vo;
        const uint16_t *line = 0;
        if (idx >= 0 && idx < TERM_SURFACE_H) line = &w->surface.cells[idx * TERM_SURFACE_W];
        else if (idx < 0) line = terminal_scrollback_row(&w->surface, -idx);
        for (int col = 0; col < cols; col++) {
            uint16_t cell = line ? line[col] : (uint16_t)(' ' | (VGA_LIGHT_GREY << 8));
            uint8_t attr = (uint8_t)(cell >> 8);
            gfx_cell(&bs, x0 + col * CW, y0 + row * CH, (uint8_t)cell, pal[attr & 15], pal[(attr >> 4) & 15]);
        }
    }
}

static void traffic_lights(int x, int y, int active, int hover)
{
    static const uint32_t col[3] = { 0xFF5F57, 0xFEBC2E, 0x28C840 };
    for (int i = 0; i < 3; i++) {
        int cx = x + 18 + i * 20, cy = y + TITLE_H / 2;
        gfx_circle(&bs, cx, cy, 6, active || hover ? col[i] : 0x4B4D57, 255);
        if (hover) {
            static const char *g[3] = { "glyph_x", "glyph_minus", "glyph_maximize-2" };
            const gfx_image_t *im = gfx_image(g[i]);
            if (im) {
                /* the 16 px glyph shrunk into the 12 px light, in a dark tint */
                for (int j = 0; j < 10; j++)
                    for (int k = 0; k < 10; k++) {
                        int sx = k * 16 / 10, sy = j * 16 / 10;
                        int a = im->rgba[(sy * 16 + sx) * 4 + 3];
                        int px = cx - 5 + k, py = cy - 5 + j;
                        if (a > 60 && px >= 0 && px < SW && py >= 0 && py < SH)
                            back[py * SW + px] = gfx_mix(back[py * SW + px], 0x4A1010, a * 3 / 4);
                    }
            }
        }
    }
}

static void draw_panel(window_t *w, int x, int y, int pw, int ph);

/* rounded bottom corners: pixels outside the arc go back to what was under the window */
static uint32_t corner_save[2][RADIUS * RADIUS];

static void save_corners(window_t *w)
{
    for (int j = 0; j < RADIUS; j++)
        for (int i = 0; i < RADIUS; i++) {
            int y = w->gy + w->gh - RADIUS + j;
            int xl = w->gx + i, xr = w->gx + w->gw - RADIUS + i;
            corner_save[0][j * RADIUS + i] = (y >= 0 && y < SH && xl >= 0 && xl < SW) ? back[y * SW + xl] : 0;
            corner_save[1][j * RADIUS + i] = (y >= 0 && y < SH && xr >= 0 && xr < SW) ? back[y * SW + xr] : 0;
        }
}

static void cut_corners(window_t *w)
{
    for (int j = 0; j < RADIUS; j++)
        for (int i = 0; i < RADIUS; i++) {
            /* coverage of pixel (i,j) in the bottom-left corner of radius RADIUS */
            int dx = RADIUS - i, dy = j + 1;
            int inside = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int fx = dx * 8 - sx * 2 - 1, fy = dy * 8 - sy * 2 - 1;
                    if (fx * fx + fy * fy <= RADIUS * RADIUS * 64) inside++;
                }
            int cov = inside * 255 / 16;
            if (cov >= 255) continue;
            int y = w->gy + w->gh - RADIUS + j;
            if (y < 0 || y >= SH) continue;
            int xl = w->gx + i, xr = w->gx + w->gw - 1 - i;
            if (xl >= 0 && xl < SW) back[y * SW + xl] = gfx_mix(corner_save[0][j * RADIUS + i], back[y * SW + xl], cov);
            if (xr >= 0 && xr < SW) back[y * SW + xr] = gfx_mix(corner_save[1][j * RADIUS + RADIUS - 1 - i], back[y * SW + xr], cov);
        }
}

static void draw_window_g(window_t *w)
{
    int x = w->gx, y = w->gy, ww = w->gw, hh = w->gh;
    int active = (w == wm_focused);
    int hover = mx >= x + 8 && mx < x + 72 && my >= y && my < y + TITLE_H;

    gfx_surface_t sc;
    gfx_surface_init(&sc, scratch, SW, SH);
    if (w->anim > 0) gfx_copy_rect(&sc, &bs, x - 40, y - 40, ww + 80, hh + 80);

    gfx_shadow(&bs, x, y, ww, hh, active ? 230 : 130);
    save_corners(w);

    /* title bar */
    gfx_rrect(&bs, x, y, ww, TITLE_H + RADIUS, RADIUS, active ? C_TITLE_ON : C_TITLE_OFF, 255);
    gfx_hline(&bs, x + RADIUS, y, ww - 2 * RADIUS, 0xFFFFFF, 26);              /* top highlight */
    traffic_lights(x, y, active, hover);
    const app_t *a = app_for_window(w);
    int tw = gfx_text_width(&ui_font_medium, w->title);
    int tx = x + (ww - tw) / 2;
    if (tx < x + 80) tx = x + 80;
    if (a) gfx_blit(&bs, icon(a->icon, 0), tx - 26, y + (TITLE_H - 20) / 2, 255);
    gfx_clip(&bs, x, y, ww - 8, TITLE_H);
    gfx_text(&bs, &ui_font_medium, tx, y + 8, w->title, active ? C_TEXT : C_TEXT_DIM, 255);
    gfx_unclip(&bs);

    /* content */
    gfx_clip(&bs, x, y + TITLE_H, ww, hh - TITLE_H);
    if (w->kind == WIN_KIND_PANEL) draw_panel(w, x, y + TITLE_H, ww, hh - TITLE_H);
    else {
        /* the grid's background colour, taken from its first cell, fills the padding too */
        uint8_t attr = (uint8_t)(w->surface.cells[0] >> 8);
        gfx_fill(&bs, x, y + TITLE_H, ww, hh - TITLE_H, pal[(attr >> 4) & 15]);
        draw_cells(w, x + CPAD_X, y + TITLE_H + CPAD_Y);
    }
    gfx_unclip(&bs);
    gfx_hline(&bs, x, y + TITLE_H, ww, 0x000000, 90);

    /* a thin scroll indicator for terminals with history */
    if (w->kind == WIN_KIND_TERMINAL && (w->surface.scrollback_count > 0 || w->surface.view_offset > 0) &&
        wm_term_max_view_offset(w) > 0) {
        int maxo = wm_term_max_view_offset(w);
        int track = hh - TITLE_H - 12;
        int th = track / 4 < 24 ? 24 : track / 4;
        int ty = y + TITLE_H + 6 + (track - th) - (track - th) * w->surface.view_offset / maxo;
        gfx_rrect(&bs, x + ww - 9, ty, 5, th, 2, 0xFFFFFF, w->surface.view_offset ? 150 : 60);
    }

    cut_corners(w);
    gfx_rrect_outline(&bs, x, y, ww, hh, RADIUS, 0xFFFFFF, 30);

    if (w->anim > 0) {
        /* fade in over the first frames */
        int alpha = 255 - w->anim * 40;
        for (int j = y - 40; j < y + hh + 40; j++) {
            if (j < 0 || j >= SH) continue;
            for (int i = x - 40; i < x + ww + 40; i++) {
                if (i < 0 || i >= SW) continue;
                back[j * SW + i] = gfx_mix(scratch[j * SW + i], back[j * SW + i], alpha);
            }
        }
        w->anim--;
        dirty = 1;
    }
}

static const char *const wdays[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void put2(char *p, int v) { p[0] = (char)('0' + v / 10 % 10); p[1] = (char)('0' + v % 10); }

static void draw_topbar(void)
{
    glass(0, 0, SW, TOPBAR_H, 0, 0x101015, 120);
    gfx_hline(&bs, 0, TOPBAR_H - 1, SW, 0x000000, 70);

    int x = 14;
    const gfx_image_t *logo = gfx_image("logo");
    if (menu_which == MENU_SYS) gfx_rrect(&bs, x - 6, 3, 30, TOPBAR_H - 6, 6, 0xFFFFFF, 40);
    gfx_blit(&bs, logo, x, (TOPBAR_H - 18) / 2, 255);
    menu_label_x[0] = x - 6; menu_label_w[0] = 30;
    x += 34;

    const app_t *a = wm_focused && wm_focused->open ? app_for_window(wm_focused) : 0;
    const char *app = a ? a->title : "Desktop";
    gfx_text(&bs, &ui_font_medium, x, 6, app, C_TEXT, 255);
    x += gfx_text_width(&ui_font_medium, app) + 22;

    static const char *labels[2] = { "File", "Window" };
    for (int i = 0; i < 2; i++) {
        int tw = gfx_text_width(&ui_font_regular, labels[i]);
        menu_label_x[i + 1] = x - 8; menu_label_w[i + 1] = tw + 16;
        if (menu_which == i + 2) gfx_rrect(&bs, x - 8, 3, tw + 16, TOPBAR_H - 6, 6, 0xFFFFFF, 40);
        gfx_text(&bs, &ui_font_regular, x, 6, labels[i], C_TEXT, 230);
        x += tw + 22;
    }

    /* right side: clock, then status icons */
    cmos_time_t t;
    cmos_get_time(&t);
    char clk[32];
    int o = 0;
    int wd = weekday(t.year, t.month, t.day);
    const char *s = wdays[wd % 7];
    while (*s) clk[o++] = *s++;
    clk[o++] = ' ';
    if (t.day >= 10) clk[o++] = (char)('0' + t.day / 10);
    clk[o++] = (char)('0' + t.day % 10);
    clk[o++] = ' ';
    s = months[(t.month - 1 + 12) % 12];
    while (*s) clk[o++] = *s++;
    clk[o++] = ' '; clk[o++] = ' ';
    put2(clk + o, t.hour); o += 2;
    clk[o++] = ':';
    put2(clk + o, t.minute); o += 2;
    clk[o] = 0;
    int cw = gfx_text_width(&ui_font_medium, clk);
    int rx = SW - 14 - cw;
    gfx_text(&bs, &ui_font_medium, rx, 6, clk, C_TEXT, 255);
    rx -= 30;
    gfx_blit(&bs, gfx_image("glyph_search"), rx, 6, 230);
    menu_label_x[3] = rx - 6; menu_label_w[3] = 28;
    rx -= 28;
    gfx_blit(&bs, gfx_image("glyph_volume-2"), rx, 6, 230);
    rx -= 28;
    gfx_blit(&bs, gfx_image("glyph_wifi"), rx, 6, net_ip ? 230 : 90);
}

/* dock layout: index 0 is the launcher, then the dock apps, then the trash */
typedef struct { int x, y, size; const app_t *app; int special; } dock_slot_t;
static dock_slot_t dock[24];
static int dock_n, dock_x, dock_w, dock_y;

static void layout_dock(void)
{
    dock_n = 0;
    dock[dock_n++] = (dock_slot_t){ 0, 0, DOCK_ICON, 0, 1 };              /* launcher */
    for (int i = 0; i < NAPPS; i++) if (apps[i].dock) dock[dock_n++] = (dock_slot_t){ 0, 0, DOCK_ICON, &apps[i], 0 };
    dock[dock_n++] = (dock_slot_t){ 0, 0, DOCK_ICON, 0, 2 };              /* trash */
    /* sizes: the icon under the pointer and its neighbours grow a little */
    for (int i = 0; i < dock_n; i++) {
        int d = hover_dock < 0 ? 99 : (i > hover_dock ? i - hover_dock : hover_dock - i);
        dock[i].size = !dock_magnify ? DOCK_ICON : d == 0 ? 60 : d == 1 ? 54 : DOCK_ICON;
    }
    int total = DOCK_PAD * 2 + 14;                                        /* 14: the separator */
    for (int i = 0; i < dock_n; i++) total += dock[i].size + (i ? DOCK_GAP : 0);
    dock_w = total;
    dock_x = (SW - total) / 2;
    dock_y = SH - DOCK_MARGIN - DOCK_H;
    int x = dock_x + DOCK_PAD;
    for (int i = 0; i < dock_n; i++) {
        if (dock[i].special == 2) x += 14;
        dock[i].x = x;
        dock[i].y = dock_y + DOCK_H - DOCK_PAD - dock[i].size;
        x += dock[i].size + DOCK_GAP;
    }
}

static int dock_hit(int x, int y)
{
    if (y < dock_y - 14 || y >= dock_y + DOCK_H) return -1;
    for (int i = 0; i < dock_n; i++)
        if (x >= dock[i].x - DOCK_GAP / 2 && x < dock[i].x + dock[i].size + DOCK_GAP / 2) return i;
    return -1;
}

static int app_running(const app_t *a)
{
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].open && app_for_window(&windows[i]) == a) return 1;
    return 0;
}

static void draw_dock(void)
{
    layout_dock();
    glass(dock_x, dock_y, dock_w, DOCK_H, 18, 0xFFFFFF, 46);
    gfx_rrect_outline(&bs, dock_x, dock_y, dock_w, DOCK_H, 18, 0xFFFFFF, 70);
    for (int i = 0; i < dock_n; i++) {
        const char *key = dock[i].special == 1 ? "launcher" : dock[i].special == 2 ? "trash" : dock[i].app->icon;
        gfx_blit_scaled(&bs, icon(key, 1), dock[i].x, dock[i].y, dock[i].size, dock[i].size, 255);
        if (dock[i].special == 2) gfx_fill_alpha(&bs, dock[i].x - 12, dock_y + 12, 1, DOCK_H - 24, 0xFFFFFF, 70);
        if (dock[i].app && app_running(dock[i].app))
            gfx_circle(&bs, dock[i].x + dock[i].size / 2, dock_y + DOCK_H - 4, 2, 0xFFFFFF, 220);
    }
    if (hover_dock >= 0 && hover_dock < dock_n && !menu_which) {
        const char *label = dock[hover_dock].special == 1 ? "Applications" :
                            dock[hover_dock].special == 2 ? "Trash" : dock[hover_dock].app->title;
        int tw = gfx_text_width(&ui_font_regular, label);
        int lx = dock[hover_dock].x + dock[hover_dock].size / 2 - (tw + 20) / 2;
        int ly = dock_y - 40;
        gfx_shadow(&bs, lx, ly, tw + 20, 26, 120);
        gfx_rrect(&bs, lx, ly, tw + 20, 26, 8, 0x2A2B33, 240);
        gfx_text(&bs, &ui_font_regular, lx + 10, ly + 5, label, C_TEXT, 255);
    }
}

static void draw_menu(void)
{
    if (!menu_which) return;
    int h = menu_height();
    gfx_shadow(&bs, menu_x, menu_y, menu_w, h, 200);
    gfx_rrect(&bs, menu_x, menu_y, menu_w, h, 9, C_MENU_BG, 245);
    gfx_rrect_outline(&bs, menu_x, menu_y, menu_w, h, 9, 0xFFFFFF, 34);
    int iy = menu_y + 4;
    for (int i = 0; i < menu_count; i++) {
        if (i == hover_menu) gfx_rrect(&bs, menu_x + 5, iy, menu_w - 10, ITEM_H, 6, C_ACCENT, 255);
        gfx_clip(&bs, menu_x, iy, menu_w - 12, ITEM_H);
        gfx_text(&bs, &ui_font_regular, menu_x + 16, iy + 5, menu_items[i].label, C_TEXT, 255);
        gfx_unclip(&bs);
        iy += ITEM_H;
        if (menu_items[i].sep_after) {
            gfx_hline(&bs, menu_x + 12, iy + 4, menu_w - 24, 0xFFFFFF, 30);
            iy += 9;
        }
    }
}

/* the launcher: every app in a grid over the frosted wallpaper */
#define L_COLS 6
#define L_CELL_W 130
#define L_CELL_H 112

static int launcher_origin(int *ox, int *oy)
{
    int rows = (NAPPS + L_COLS - 1) / L_COLS;
    *ox = (SW - L_COLS * L_CELL_W) / 2;
    *oy = (SH - rows * L_CELL_H) / 2 + 10;
    return rows;
}

static int launcher_hit(int x, int y)
{
    int ox, oy;
    launcher_origin(&ox, &oy);
    if (x < ox || y < oy) return -1;
    int c = (x - ox) / L_CELL_W, r = (y - oy) / L_CELL_H;
    if (c >= L_COLS) return -1;
    int i = r * L_COLS + c;
    return i < NAPPS ? i : -1;
}

static void draw_launcher(void)
{
    memcpy(back, wall_blur, (size_t)SW * SH * 4);
    gfx_fill_alpha(&bs, 0, 0, SW, SH, 0x000000, 90);
    int ox, oy;
    launcher_origin(&ox, &oy);
    gfx_text_center(&bs, &ui_font_title, 0, oy - 70, SW, "Applications", C_TEXT, 255);
    for (int i = 0; i < NAPPS; i++) {
        int cx = ox + (i % L_COLS) * L_CELL_W, cy = oy + (i / L_COLS) * L_CELL_H;
        if (i == hover_launch) gfx_rrect(&bs, cx + 10, cy, L_CELL_W - 20, L_CELL_H - 10, 14, 0xFFFFFF, 40);
        gfx_blit_scaled(&bs, icon(apps[i].icon, 1), cx + (L_CELL_W - 64) / 2, cy + 10, 64, 64, 255);
        gfx_text_center(&bs, &ui_font_regular, cx, cy + 80, L_CELL_W, apps[i].title, C_TEXT, 255);
    }
    gfx_text_center(&bs, &ui_font_small, 0, SH - 40, SW, "Click an app to open it, anywhere else to close", C_TEXT_DIM, 255);
}

/* ------------------------------------------------------------------ panels */

static const int res_w[5] = { 1024, 1280, 1366, 1600, 1920 };
static const int res_h[5] = { 768, 720, 768, 900, 1080 };

static int set_resolution(int w, int h);

static void button(int x, int y, int w, int h, const char *label, int on)
{
    gfx_rrect(&bs, x, y, w, h, 7, on ? C_ACCENT : 0x3A3C46, 255);
    gfx_text_center(&bs, &ui_font_regular, x, y + (h - 17) / 2, w, label, C_TEXT, 255);
}

static void section(int x, int y, const char *t)
{
    gfx_text(&bs, &ui_font_small, x, y, t, C_TEXT_DIM, 255);
}

static void draw_settings(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, 0x1E1F25);
    int px = x + 24, py = y + 18;
    gfx_text(&bs, &ui_font_title, px, py, "Settings", C_TEXT, 255);
    py += 44;

    section(px, py, "DISPLAY RESOLUTION"); py += 22;
    for (int i = 0; i < 5; i++) {
        char lbl[16];
        int o = 0, v = res_w[i];
        char tmp[8]; int n = 0;
        while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) lbl[o++] = tmp[--n];
        lbl[o++] = 'x';
        v = res_h[i];
        while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) lbl[o++] = tmp[--n];
        lbl[o] = 0;
        button(px + i * 112, py, 104, 32, lbl, SW == res_w[i] && SH == res_h[i]);
    }
    py += 54;

    section(px, py, "WALLPAPER"); py += 22;
    static const char *wn[3] = { "Dusk", "Ocean", "Graphite" };
    static const uint32_t wa[3][2] = { { 0x302468, 0xE07A6E }, { 0x0B3A66, 0x2BB3C9 }, { 0x2A2C33, 0x5A5F6B } };
    for (int i = 0; i < 3; i++) {
        int bx = px + i * 150;
        gfx_rrect(&bs, bx - 3, py - 3, 136, 82, 10, wallpaper_choice == i ? C_ACCENT : 0x1E1F25, 255);
        gfx_surface_t tmp = bs;
        gfx_clip(&tmp, bx, py, 130, 76);
        gfx_gradient_v(&tmp, bx, py, 130, 76, wa[i][0], wa[i][1]);
        gfx_text_center(&bs, &ui_font_regular, bx, py + 84, 130, wn[i], C_TEXT, 255);
    }
    py += 118;

    section(px, py, "DOCK"); py += 22;
    button(px, py, 220, 32, dock_magnify ? "Magnification: On" : "Magnification: Off", dock_magnify);
    py += 54;

    section(px, py, "POINTER SPEED"); py += 22;
    static const char *sp[3] = { "Slow", "Normal", "Fast" };
    for (int i = 0; i < 3; i++) button(px + i * 112, py, 104, 32, sp[i], mouse_speed == i);
    (void)h;
}

static void click_settings(int lx, int ly)
{
    int px = 24, py = 18 + 44 + 22;
    for (int i = 0; i < 5; i++)
        if (lx >= px + i * 112 && lx < px + i * 112 + 104 && ly >= py && ly < py + 32) { set_resolution(res_w[i], res_h[i]); return; }
    py += 54 + 22;
    for (int i = 0; i < 3; i++)
        if (lx >= px + i * 150 && lx < px + i * 150 + 130 && ly >= py && ly < py + 76) { set_wallpaper(i); return; }
    py += 118 + 22;
    if (lx >= px && lx < px + 220 && ly >= py && ly < py + 32) { dock_magnify = !dock_magnify; return; }
    py += 54 + 22;
    for (int i = 0; i < 3; i++)
        if (lx >= px + i * 112 && lx < px + i * 112 + 104 && ly >= py && ly < py + 32) { mouse_speed = i; return; }
}

static void fmt_num(char *out, uint32_t v)
{
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int o = 0;
    while (n) out[o++] = t[--n];
    out[o] = 0;
}

static void draw_about(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, 0x1E1F25);
    const gfx_image_t *lg = icon("launcher", 1);
    gfx_blit_scaled(&bs, lg, x + (w - 72) / 2, y + 24, 72, 72, 255);
    gfx_text_center(&bs, &ui_font_huge, x, y + 100, w, "tOS", C_TEXT, 255);
    gfx_text_center(&bs, &ui_font_regular, x, y + 162, w, "A hobby operating system, from boot sector to desktop", C_TEXT_DIM, 255);

    char line[64], num[16];
    uint32_t total = 0, used = 0;
    memory_get_usage(&total, &used);
    int ly = y + 200;
    strcpy(line, "Display   ");
    fmt_num(num, (uint32_t)SW); strcat(line, num); strcat(line, " x ");
    fmt_num(num, (uint32_t)SH); strcat(line, num);
    gfx_text_center(&bs, &ui_font_regular, x, ly, w, line, C_TEXT, 255); ly += 24;
    strcpy(line, "Memory    ");
    fmt_num(num, used / 1024); strcat(line, num); strcat(line, " MB used of ");
    fmt_num(num, total / 1024); strcat(line, num); strcat(line, " MB");
    gfx_text_center(&bs, &ui_font_regular, x, ly, w, line, C_TEXT, 255); ly += 24;
    uint32_t up = debugmon_uptime_ms() / 1000;
    strcpy(line, "Uptime    ");
    fmt_num(num, up / 60); strcat(line, num); strcat(line, " min ");
    fmt_num(num, up % 60); strcat(line, num); strcat(line, " s");
    gfx_text_center(&bs, &ui_font_regular, x, ly, w, line, C_TEXT, 255); ly += 24;
    strcpy(line, "Tasks     ");
    fmt_num(num, task_count()); strcat(line, num);
    gfx_text_center(&bs, &ui_font_regular, x, ly, w, line, C_TEXT, 255);
    gfx_text_center(&bs, &ui_font_small, x, y + h - 30, w, "Inter & JetBrains Mono (SIL OFL)  ·  Lucide icons (ISC)", C_TEXT_DIM, 255);
    dirty = 1;               /* the uptime keeps moving */
}

static void draw_panel(window_t *w, int x, int y, int pw, int ph)
{
    if (w->panel == PANEL_SETTINGS) draw_settings(x, y, pw, ph);
    else draw_about(x, y, pw, ph);
}

/* ------------------------------------------------------------------ frames */

static void compose(void)
{
    if (launcher_open) { draw_launcher(); return; }
    draw_wallpaper();
    int order[MAX_WINDOWS], n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].open && !windows[i].minimized) order[n++] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && windows[order[j]].z > windows[k].z) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    for (int i = 0; i < n; i++) draw_window_g(&windows[order[i]]);
    draw_dock();
    draw_topbar();
    draw_menu();
}

static void cursor_erase(void)
{
    if (cur_x < -50) return;
    for (int j = 0; j < 26; j++) {
        int y = cur_y + j;
        if (y < 0 || y >= SH) continue;
        int x0 = cur_x < 0 ? 0 : cur_x, x1 = cur_x + 24 > SW ? SW : cur_x + 24;
        if (x1 > x0) memcpy(lfb + y * SW + x0, back + y * SW + x0, (size_t)(x1 - x0) * 4);
    }
}

static void cursor_draw(void)
{
    const gfx_image_t *c = gfx_image("cursor");
    if (!c) return;
    int ox = mx - 1, oy = my - 1;          /* the arrow's tip sits one pixel in */
    for (int j = 0; j < c->h; j++) {
        int y = oy + j;
        if (y < 0 || y >= SH) continue;
        for (int i = 0; i < c->w; i++) {
            int x = ox + i;
            if (x < 0 || x >= SW) continue;
            const uint8_t *p = c->rgba + (j * c->w + i) * 4;
            if (p[3]) lfb[y * SW + x] = gfx_mix(back[y * SW + x], gfx_rgb(p[0], p[1], p[2]), p[3]);
        }
    }
    cur_x = ox; cur_y = oy;
}

static void present(void)
{
    memcpy(lfb, back, (size_t)SW * SH * 4);
    cur_x = -100;
    cursor_draw();
}

static void shutdown_screen(void)
{
    gfx_fill(&bs, 0, 0, SW, SH, 0x000000);
    gfx_blit_scaled(&bs, icon("launcher", 1), (SW - 72) / 2, SH / 2 - 110, 72, 72, 255);
    gfx_text_center(&bs, &ui_font_title, 0, SH / 2 - 20, SW, "tOS", C_TEXT, 255);
    gfx_text_center(&bs, &ui_font_regular, 0, SH / 2 + 20, SW, "It is now safe to turn off your computer.", C_TEXT_DIM, 255);
    memcpy(lfb, back, (size_t)SW * SH * 4);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ------------------------------------------------------------------ wallpaper, mode */

static void set_wallpaper(int choice)
{
    wallpaper_choice = choice;
    if (choice == 0) gfx_wallpaper(wall, SW, SH, wall_blur);
    else {
        uint32_t top = choice == 1 ? 0x0B3A66 : 0x2A2C33, bot = choice == 1 ? 0x2BB3C9 : 0x5A5F6B;
        gfx_surface_t ws;
        gfx_surface_init(&ws, wall, SW, SH);
        gfx_gradient_v(&ws, 0, 0, SW, SH, top, bot);
        /* a soft light from the upper left */
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                int dx = x - SW / 4, dy = y - SH / 5;
                int d = (dx * dx + dy * dy) / (SW * 4);
                int a = 70 - d;
                if (a > 0) wall[y * SW + x] = gfx_mix(wall[y * SW + x], 0xFFFFFF, a);
            }
        memcpy(wall_blur, wall, (size_t)SW * SH * 4);      /* already smooth */
    }
    dirty = 1;
}

static int alloc_buffers(int w, int h)
{
    size_t n = (size_t)w * h * 4;
    uint32_t *b = malloc(n), *wl = malloc(n), *bl = malloc(n), *sc = malloc(n);
    if (!b || !wl || !bl || !sc) {
        if (b) free(b);
        if (wl) free(wl);
        if (bl) free(bl);
        if (sc) free(sc);
        return -1;
    }
    if (back) free(back);
    if (wall) free(wall);
    if (wall_blur) free(wall_blur);
    if (scratch) free(scratch);
    back = b; wall = wl; wall_blur = bl; scratch = sc;
    gfx_surface_init(&bs, back, w, h);
    return 0;
}

static int set_resolution(int w, int h)
{
    if (w == SW && h == SH) return 0;
    int oldw = SW, oldh = SH;
    if (bochs_set_mode(&vdev, w, h, 32) != 0) return -1;
    if (alloc_buffers(w, h) != 0) { bochs_set_mode(&vdev, oldw, oldh, 32); return -1; }
    SW = w; SH = h;
    set_wallpaper(wallpaper_choice);
    mouse_set_pixel_mode(w, h);
    mx = w / 2; my = h / 2;
    for (int i = 0; i < MAX_WINDOWS; i++) if (windows[i].open) clamp_window(&windows[i]);
    dirty = 1;
    return 0;
}

void wm_gfx_restore_mode(void)
{
    bochs_set_mode(&vdev, SW, SH, 32);
    mouse_set_pixel_mode(SW, SH);
    dirty = 1;
}

int wm_gfx_init(void)
{
    if (bochs_init(&vdev) != 0 || !vdev.lfb) return -1;
    if (gfx_init() != 0) return -1;
    /* bochs_init maps 4 MB of the framebuffer; 1920x1080 needs 8 */
    paging_map_range(vdev.lfb, vdev.lfb, 0x1000000, PTE_PRESENT | PTE_WRITABLE);
    SW = 1280; SH = 720;                        /* the first-boot mode */
    if (alloc_buffers(SW, SH) != 0) return -1;
    if (bochs_set_mode(&vdev, SW, SH, 32) != 0) return -1;
    lfb = (uint32_t *)vdev.lfb;
    set_wallpaper(0);
    mouse_set_pixel_mode(SW, SH);
    mx = SW / 2; my = SH / 2;
    dirty = 1;
    return 0;
}

/* ------------------------------------------------------------------ input */

/* Under QEMU, the debug console (port 0xE9, which reads back 0xE9 there and nothing on real
 * hardware) gets the pointer position, so automated tests can steer the mouse exactly. */
static void debugcon_pointer(void)
{
    static int present = -1;
    if (present < 0) present = inb(0xE9) == 0xE9;
    if (!present) return;
    char b[24];
    int o = 0;
    b[o++] = 'P'; b[o++] = ' ';
    char t[8]; int n;
    int v = mx; n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) b[o++] = t[--n];
    b[o++] = ' ';
    v = my; n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) b[o++] = t[--n];
    b[o++] = '\n';
    for (int i = 0; i < o; i++) outb(0xE9, (uint8_t)b[i]);
}

static window_t *window_at(int x, int y)
{
    window_t *hit = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->open || w->minimized) continue;
        if (x >= w->gx && x < w->gx + w->gw && y >= w->gy && y < w->gy + w->gh)
            if (!hit || w->z > hit->z) hit = w;
    }
    return hit;
}

static void press(int x, int y)
{
    /* menus first: a click inside chooses, anywhere else closes */
    if (menu_which) {
        int i = menu_hit(x, y);
        int was = menu_which;
        menu_which = 0;
        dirty = 1;
        if (i >= 0) { sound_click(); do_action(menu_items[i].action); return; }
        if (y < TOPBAR_H) {
            for (int k = 0; k < 3; k++)
                if (x >= menu_label_x[k] && x < menu_label_x[k] + menu_label_w[k] && was == k + 1) return;
        } else return;
    }
    if (launcher_open) {
        int i = launcher_hit(x, y);
        launcher_open = 0;
        dirty = 1;
        if (i >= 0) launch(&apps[i]);
        return;
    }
    if (y < TOPBAR_H) {
        static const int which[3] = { MENU_SYS, MENU_FILE_, MENU_WINDOW };
        for (int k = 0; k < 3; k++)
            if (x >= menu_label_x[k] && x < menu_label_x[k] + menu_label_w[k]) {
                open_menu(which[k], menu_label_x[k], TOPBAR_H + 2);
                return;
            }
        if (x >= menu_label_x[3] && x < menu_label_x[3] + menu_label_w[3]) { launcher_open = 1; dirty = 1; }
        return;
    }
    int d = dock_hit(x, y);
    if (d >= 0) {
        if (dock[d].special == 1) { launcher_open = 1; sound_click(); }
        else if (dock[d].special == 2) wm_open_app("files");
        else dock_activate(dock[d].app);
        dirty = 1;
        return;
    }
    window_t *w = window_at(x, y);
    if (!w) return;
    int was_focused = (w == wm_focused);
    wm_focus_window(w);
    dirty = 1;
    int lx = x - w->gx, ly = y - w->gy;
    if (ly < TITLE_H) {
        for (int i = 0; i < 3; i++) {
            int cx = 18 + i * 20, cy = TITLE_H / 2;
            if ((lx - cx) * (lx - cx) + (ly - cy) * (ly - cy) <= 64) {
                sound_click();
                if (i == 0) wm_close_window(w);
                else if (i == 1) w->minimized = 1;
                else zoom_window(w);
                return;
            }
        }
        uint32_t now = debugmon_uptime_ms();
        if (last_title_click_win == w && now - last_title_click_ms < 400) { zoom_window(w); last_title_click_win = 0; return; }
        last_title_click_win = w; last_title_click_ms = now;
        drag_win = w; drag_dx = lx; drag_dy = ly;
        return;
    }
    if (w->kind != WIN_KIND_PANEL && !w->maximized && lx >= w->gw - 16 && ly >= w->gh - 16) {
        resize_win = w;
        return;
    }
    if (w->kind == WIN_KIND_PANEL) {
        if (w->panel == PANEL_SETTINGS) click_settings(lx, ly - TITLE_H);
        return;
    }
    if (w->kind == WIN_KIND_TERMINAL && wm_term_max_view_offset(w) > 0 && lx >= w->gw - 12) {
        int track = w->gh - TITLE_H;
        int maxo = wm_term_max_view_offset(w);
        int vo = maxo - (ly - TITLE_H) * maxo / (track > 1 ? track : 1);
        w->surface.view_offset = vo < 0 ? 0 : vo > maxo ? maxo : vo;
        return;
    }
    if (was_focused) {
        content_click_target = w;
        int cx2 = lx - CPAD_X, cy2 = ly - TITLE_H - CPAD_Y;
        content_click_x = cx2 >= 0 ? cx2 / CW : -1;
        content_click_y = cy2 >= 0 ? cy2 / CH : -1;
    }
}

void wm_gfx_tick(void)
{
    /* another program (DOOM, a graphics .t program, vgatest) has the display: stay out of its
     * way, and repaint everything once it gives the display back */
    if (bochs_graphics_active()) { ext_graphics = 1; return; }
    if (ext_graphics) { ext_graphics = 0; wm_gfx_restore_mode(); }

    mouse_poll();
    int nx, ny;
    uint8_t btn;
    mouse_get_state(&nx, &ny, &btn);
    if (mouse_speed != 1) {
        /* scale motion relative to where the pointer was */
        int dx = nx - mx, dy = ny - my;
        if (mouse_speed == 0) { dx /= 2; dy /= 2; } else { dx = dx * 3 / 2; dy = dy * 3 / 2; }
        nx = mx + dx; ny = my + dy;
        if (nx < 0) nx = 0;
        if (nx >= SW) nx = SW - 1;
        if (ny < 0) ny = 0;
        if (ny >= SH) ny = SH - 1;
    }
    int moved = nx != mx || ny != my;
    mx = nx; my = ny;
    if (moved) debugcon_pointer();

    /* clicks the driver latched between two frames still count, even if the button is already
     * up again by the time it is looked at */
    int cx, cy, rcx, rcy;
    int latched = mouse_get_click(&cx, &cy);
    int rlatched = mouse_get_rclick(&rcx, &rcy);
    int pressed = (btn & 1) && !(prev_btn & 1);
    if (!pressed && latched && !(prev_btn & 1)) pressed = 1;
    int released = !(btn & 1) && (prev_btn & 1);
    int rpressed = ((btn & 2) && !(prev_btn & 2)) || (rlatched && !(prev_btn & 2));
    prev_btn = btn;

    if (drag_win) {
        if (btn & 1) {
            drag_win->gx = mx - drag_dx;
            drag_win->gy = my - drag_dy;
            clamp_window(drag_win);
            if (moved) dirty = 1;
        } else drag_win = 0;
    } else if (resize_win) {
        if (btn & 1) {
            int cols = (mx - resize_win->gx - 2 * CPAD_X + CW / 2) / CW;
            int rows = (my - resize_win->gy - TITLE_H - 2 * CPAD_Y + CH / 2) / CH;
            int oc = resize_win->w0, orow = resize_win->h0;
            set_cells(resize_win, cols, rows);
            if (oc != resize_win->w0 || orow != resize_win->h0) dirty = 1;
        } else resize_win = 0;
    } else if (pressed) {
        press(mx, my);
    }
    (void)released;

    if (rpressed && !menu_which && !launcher_open && my >= TOPBAR_H && dock_hit(mx, my) < 0 && !window_at(mx, my))
        open_menu(MENU_CTX, mx, my);

    int wheel = mouse_get_wheel_delta();
    if (wheel) {
        window_t *u = window_at(mx, my);
        if (u && u->kind == WIN_KIND_TERMINAL && wm_term_max_view_offset(u) > 0) {
            int maxo = wm_term_max_view_offset(u);
            int vo = u->surface.view_offset + wheel * 2;
            u->surface.view_offset = vo < 0 ? 0 : vo > maxo ? maxo : vo;
            dirty = 1;
        } else if (u) { wheel_target = u; pending_wheel_delta = wheel; }
    }

    /* hover states that change the picture */
    if (moved) {
        layout_dock();
        int hd = (!menu_which && !launcher_open && !drag_win) ? dock_hit(mx, my) : -1;
        if (hd != hover_dock) { hover_dock = hd; dirty = 1; }
        int hm = menu_hit(mx, my);
        if (hm != hover_menu) { hover_menu = hm; dirty = 1; }
        int hl = launcher_open ? launcher_hit(mx, my) : -1;
        if (hl != hover_launch) { hover_launch = hl; dirty = 1; }
        window_t *top = window_at(mx, my);
        int lights = top && mx >= top->gx + 8 && mx < top->gx + 72 && my >= top->gy && my < top->gy + TITLE_H;
        if (lights != hover_lights) { hover_lights = lights; dirty = 1; }
    }

    /* window contents that changed since they were last drawn */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->open || w->minimized || w->kind == WIN_KIND_PANEL) continue;
        uint32_t s = surface_sum(w);
        if (s != w->g_sum) { w->g_sum = s; dirty = 1; }
    }

    cmos_time_t t;
    cmos_get_time(&t);
    if (t.minute != last_minute) { last_minute = t.minute; dirty = 1; }

    uint32_t now = debugmon_uptime_ms();
    if (dirty && now - last_frame_ms >= 16) {
        last_frame_ms = now;
        dirty = 0;
        compose();
        present();
    } else if (moved) {
        cursor_erase();
        cursor_draw();
    }
}
