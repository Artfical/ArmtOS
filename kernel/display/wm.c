#include "wm.h"
#include "isr.h"
#include "terminal.h"
#include "mouse.h"
#include "scheduler.h"
#include "shell.h"
#include "string.h"
#include "cmos.h"
#include "net.h"
#include "notepad.h"
#include "clock.h"
#include "about.h"
#include "diskmgr.h"
#include "calculator.h"
#include "filemgr.h"
#include "paint.h"
#include "viewer.h"
#include "taskmgr.h"
#include "mediaplayer.h"
#include "netmon.h"
#include "snake.h"
#include "game2048.h"
#include "pdfview.h"
#include "stickynotes.h"
/* kernel/doom/port/doom_main.c -- declared here instead of pulled in
 * via a shared header since kernel/display/ isn't on DOOM_CFLAGS's
 * include path (and doesn't need to be, for one function). */
void doom_window_run(void);
#include "bochs.h"
#include "vga.h"
#include "fsbridge.h"
#include "sound.h"
#include "png.h"
#include "memory.h"
#include "wm_internal.h"

#define WM_SCREEN_W 80
#define WM_SCREEN_H 25
#define MENU_ROW 0
#define DOCK_ROW (WM_SCREEN_H - 1)

static uint16_t *const VGA_MEM = (uint16_t *)0xB8000;
static uint16_t backbuffer[WM_SCREEN_W * WM_SCREEN_H];

static window_t *drag_window;
static int drag_offset_x, drag_offset_y;

#define MIN_WIN_W 20
#define MIN_WIN_H 6
static window_t *resize_window;
static int resize_start_w, resize_start_h, resize_start_mx, resize_start_my;

window_t windows[MAX_WINDOWS];
window_t *wm_focused = NULL;
int wm_gfx_mode;
static window_t *pending_window;
int next_z = 1;

/* One-shot mouse click within a window's content area (below the titlebar),
 * in surface-local coordinates, claimable by that window's own task via
 * wm_get_content_click(). Cleared on read or overwritten by the next click. */
window_t *content_click_target = NULL;
int content_click_x, content_click_y;

/* One-shot scroll-wheel delta for whichever window is under the mouse
 * cursor, claimable by that window's own task via wm_get_content_wheel().
 * Terminal windows are scrolled directly here instead, since their
 * scrollback view_offset already lives in wm.c. */
window_t *wheel_target = NULL;
int pending_wheel_delta;

/* Highest valid view_offset for a terminal window: scrollback_count
 * plus however many extra rows the surface (TERM_SURFACE_H) has over
 * the window's visible content height — those extra rows are still
 * inside w->surface.cells, not yet pushed into scrollback, but are
 * just as much "history" once the window is shorter than the full
 * surface. */
int wm_term_max_view_offset(window_t *w);
static int term_max_view_offset(window_t *w)
{
    int content_h = w->h0 - 1;
    int extra = TERM_SURFACE_H - content_h;
    if (extra < 0) extra = 0;
    return w->surface.scrollback_count + extra;
}

int wm_term_max_view_offset(window_t *w) { return term_max_view_offset(w); }

/* One-shot menu action requested for a window via the top File/Edit/... bar,
 * claimable by that window's own task via wm_get_menu_action(). */
window_t *action_target = NULL;
int pending_action = WM_ACTION_NONE;

/* Top menu bar: T (apple-logo stand-in) + File/Edit/View/Label/Special */
enum { MENU_NONE = 0, MENU_T, MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_LABEL, MENU_SPECIAL };
static int active_menu = MENU_NONE;

/* Right-click context menu on the empty desktop (not over any window).
 * Anchored at the click point rather than the top menu bar, so it gets
 * its own small renderer instead of reusing draw_simple_menu(). */
#define CTX_NUM_ITEMS 7
static const char *ctx_labels[CTX_NUM_ITEMS] = {
    "New Folder", "New File", "Open Files", "Open Terminal",
    "About This Computer...", "Change Background", "Refresh Desktop"
};
static int ctx_open;
static int ctx_x0, ctx_y0, ctx_w;
static int ctx_item_y0[CTX_NUM_ITEMS];

static const uint8_t desktop_bg_palette[] = {
    VGA_WHITE, VGA_LIGHT_CYAN, VGA_LIGHT_GREEN, VGA_LIGHT_BLUE, VGA_LIGHT_GREY
};
#define DESKTOP_BG_COUNT (int)(sizeof(desktop_bg_palette) / sizeof(desktop_bg_palette[0]))
static int desktop_bg_idx = 0;

/* Wallpaper: a per-desktop-cell VGA color index, precomputed once when
 * set (from a decoded PNG's RGB pixels, or directly from another app's
 * own VGA-indexed canvas, e.g. Paint's "set as wallpaper" shortcut) so
 * the per-frame desktop redraw is just a cheap array lookup rather than
 * re-sampling/re-quantizing an image every frame. */
static uint8_t wallpaper_color[WM_SCREEN_H][WM_SCREEN_W];
static int wallpaper_active = 0;

static const uint8_t wallpaper_vga_palette[16][3] = {
    {0, 0, 0}, {0, 0, 170}, {0, 170, 0}, {0, 170, 170},
    {170, 0, 0}, {170, 0, 170}, {170, 85, 0}, {170, 170, 170},
    {85, 85, 85}, {85, 85, 255}, {85, 255, 85}, {85, 255, 255},
    {255, 85, 85}, {255, 85, 255}, {255, 255, 85}, {255, 255, 255},
};

static uint8_t wallpaper_nearest_color(uint8_t r, uint8_t g, uint8_t b)
{
    int best = 0, best_dist = -1;
    for (int i = 0; i < 16; i++) {
        int dr = r - wallpaper_vga_palette[i][0];
        int dg = g - wallpaper_vga_palette[i][1];
        int db = b - wallpaper_vga_palette[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (best_dist < 0 || dist < best_dist) { best_dist = dist; best = i; }
    }
    return (uint8_t)best;
}

/* Used by other apps (currently Paint) to set the desktop wallpaper
 * directly from their own already-VGA-indexed canvas, without needing
 * to save/reload a file. `cells` is a row-major rows*cols array of VGA
 * color indices (0-15). */
void wm_set_wallpaper_from_cells(const uint8_t *cells, int rows, int cols)
{
    if (rows <= 0 || cols <= 0) return;
    int desktop_rows = DOCK_ROW - (MENU_ROW + 1);
    for (int dy = 0; dy < desktop_rows; dy++) {
        int sy = dy * rows / desktop_rows;
        if (sy >= rows) sy = rows - 1;
        for (int dx = 0; dx < WM_SCREEN_W; dx++) {
            int sx = dx * cols / WM_SCREEN_W;
            if (sx >= cols) sx = cols - 1;
            wallpaper_color[MENU_ROW + 1 + dy][dx] = cells[sy * cols + sx] & 0x0F;
        }
    }
    wallpaper_active = 1;
}

/* General wallpaper support: decodes a PNG file and quantizes it down
 * to the desktop's per-cell VGA color grid. Returns 0 on success. */
int wm_set_wallpaper_file(const char *path)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return -1;
    uint32_t sz = fsbridge_size(path);
    if (!sz) return -1;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) return -1;
    fsbridge_read(path, buf, sz, 0);

    uint8_t *rgb = NULL;
    uint32_t iw = 0, ih = 0;
    char err[80];
    int rc = png_decode(buf, sz, &rgb, &iw, &ih, err, sizeof(err));
    free(buf);
    if (rc != 0 || !rgb || iw == 0 || ih == 0) { if (rgb) free(rgb); return -1; }

    int desktop_rows = DOCK_ROW - (MENU_ROW + 1);
    for (int dy = 0; dy < desktop_rows; dy++) {
        uint32_t sy = (uint32_t)((uint64_t)dy * ih / (uint32_t)desktop_rows);
        if (sy >= ih) sy = ih - 1;
        for (int dx = 0; dx < WM_SCREEN_W; dx++) {
            uint32_t sx = (uint32_t)((uint64_t)dx * iw / (uint32_t)WM_SCREEN_W);
            if (sx >= iw) sx = iw - 1;
            const uint8_t *px = rgb + ((uint64_t)sy * iw + sx) * 3;
            wallpaper_color[MENU_ROW + 1 + dy][dx] = wallpaper_nearest_color(px[0], px[1], px[2]);
        }
    }
    free(rgb);
    wallpaper_active = 1;
    return 0;
}

static int t_x0, t_x1;
static int file_x0, file_x1;
static int edit_x0, edit_x1;
static int view_x0, view_x1;
static int label_x0, label_x1;
static int special_x0, special_x1;

#define MENU_MAX_ITEMS 64
static int menu_x0[MENU_MAX_ITEMS];
static int menu_y0[MENU_MAX_ITEMS];
static int menu_x1[MENU_MAX_ITEMS];
static int menu_count = 0;
static const char *menu_names[MENU_MAX_ITEMS];
static int menu_is_app[MENU_MAX_ITEMS];
static int menu_disabled[MENU_MAX_ITEMS];

static uint8_t mk_color(uint8_t fg, uint8_t bg) { return fg | (bg << 4); }
static uint16_t mk_cell(char c, uint8_t color) { return (uint16_t)(unsigned char)c | ((uint16_t)color << 8); }

static void vga_put(int x, int y, char c, uint8_t color)
{
    if (x < 0 || x >= WM_SCREEN_W || y < 0 || y >= WM_SCREEN_H) return;
    backbuffer[y * WM_SCREEN_W + x] = mk_cell(c, color);
}

static void vga_fill_rect(int x0, int y0, int w, int h, char c, uint8_t color)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            vga_put(x, y, c, color);
}

static void vga_text(int x, int y, const char *s, uint8_t color)
{
    int i = 0;
    while (s[i]) { vga_put(x + i, y, s[i], color); i++; }
}

int wm_current_task_has_focus(void)
{
    void *ud = task_get_userdata();
    if (!ud) return 1;
    return ud == (void *)wm_focused;
}

int wm_get_content_click(int *x, int *y)
{
    window_t *self = (window_t *)task_get_userdata();
    if (!self || content_click_target != self) return 0;
    content_click_target = NULL;
    if (x) *x = content_click_x;
    if (y) *y = content_click_y;
    return 1;
}

/* Positive = wheel scrolled up (toward the user's content history),
 * negative = scrolled down. Zero if no wheel motion is pending for the
 * calling task's own window. */
int wm_get_content_wheel(void)
{
    window_t *self = (window_t *)task_get_userdata();
    if (!self || wheel_target != self) return 0;
    wheel_target = NULL;
    int d = pending_wheel_delta;
    pending_wheel_delta = 0;
    return d;
}

/* Continuous (per-frame) content-relative mouse position + live button
 * mask for the focused window's own task, used for drag gestures (e.g.
 * Paint) where a one-shot click isn't enough. */
int wm_get_content_mouse(int *x, int *y, int *buttons)
{
    window_t *self = (window_t *)task_get_userdata();
    if (!self || wm_focused != self) return 0;
    int mx, my;
    uint8_t btns;
    mouse_get_state(&mx, &my, &btns);
    if (wm_gfx_mode) {
        wm_gfx_content_mouse(self, x, y);
        if (buttons) *buttons = btns;
        return 1;
    }
    if (x) *x = mx - self->x0;
    if (y) *y = my - self->y0 - 1;
    if (buttons) *buttons = btns;
    return 1;
}

int wm_get_menu_action(void)
{
    window_t *self = (window_t *)task_get_userdata();
    if (!self || action_target != self || pending_action == WM_ACTION_NONE) return WM_ACTION_NONE;
    int a = pending_action;
    action_target = NULL;
    pending_action = WM_ACTION_NONE;
    return a;
}

/* Computes the initial position/size for a newly opened window (called
 * once at creation, not on every redraw) and stores both the active
 * geometry and the "restore" geometry to return to when un-maximized.
 * Windows are freely draggable afterwards by their title bar; this only
 * ever runs again when a window is opened. */
static void window_geom_init(window_t *w)
{
    int slot = (int)(w - windows);
    w->rest_x0 = 4 + (slot % 4) * 3;
    w->rest_y0 = 2 + (slot % 3) * 2;
    w->rest_w0 = NORMAL_W;
    w->rest_h0 = NORMAL_H;
    if (w->maximized) {
        w->x0 = 0; w->y0 = MENU_ROW + 1; w->w0 = WM_SCREEN_W; w->h0 = WM_SCREEN_H - 2;
    } else {
        w->x0 = w->rest_x0; w->y0 = w->rest_y0; w->w0 = w->rest_w0; w->h0 = w->rest_h0;
    }
    if (wm_gfx_mode) wm_gfx_window_opened(w);
    sound_open();
}

static int fmt_uint(char *buf, unsigned int v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static int fmt_uint02(char *buf, unsigned int v)
{
    buf[0] = '0' + (v / 10) % 10;
    buf[1] = '0' + (v % 10);
    return 2;
}

int wm_find_free_slot(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].open) return i;
    return -1;
}

static void window_task_entry(void)
{
    window_t *w = pending_window;
    task_set_userdata(w);
    if (w->kind == WIN_KIND_NOTEPAD)
        notepad_run();
    else if (w->kind == WIN_KIND_CLOCK)
        clock_run();
    else if (w->kind == WIN_KIND_ABOUT)
        about_run();
    else if (w->kind == WIN_KIND_DISKMGR)
        diskmgr_run();
    else if (w->kind == WIN_KIND_CALCULATOR)
        calculator_run();
    else if (w->kind == WIN_KIND_FILEMGR)
        filemgr_run();
    else if (w->kind == WIN_KIND_PAINT)
        paint_run();
    else if (w->kind == WIN_KIND_VIEWER)
        viewer_run();
    else if (w->kind == WIN_KIND_TASKMGR)
        taskmgr_run();
    else if (w->kind == WIN_KIND_MEDIAPLAYER)
        mediaplayer_run();
    else if (w->kind == WIN_KIND_NETMON)
        netmon_run();
    else if (w->kind == WIN_KIND_SNAKE)
        snake_run();
    else if (w->kind == WIN_KIND_2048)
        game2048_run();
    else if (w->kind == WIN_KIND_PDFVIEWER)
        pdfview_run();
    else if (w->kind == WIN_KIND_STICKYNOTES)
        stickynotes_run();
    else if (w->kind == WIN_KIND_DOOM)
        doom_window_run();
    else
        shell_run_windowed(w->initial_cmd);
}

void wm_focus_window(window_t *w)
{
    w->minimized = 0;
    w->z = next_z++;
    wm_focused = w;
}

void wm_close_window(window_t *w)
{
    /* DOOM's window runs in real VBE/Bochs pixel graphics (see
     * doomgeneric_tos.c's DG_Init()/DG_DrawFrame()) instead of the
     * desktop's normal VGA text mode, and task_kill() below is an
     * abrupt kill -- it doesn't let doom_window_run() return and hit
     * any cleanup of its own. Without switching back here first, the
     * hardware would be left stuck in graphics mode showing DOOM's
     * last frame forever after the window "closes". This mirrors what
     * cmd_vgatest already does to leave graphics mode. */
    if (w->kind == WIN_KIND_DOOM && wm_gfx_mode) {
        terminal_set_force_direct(0);
        bochs_set_graphics_active(0);
        wm_gfx_restore_mode();
    } else if (w->kind == WIN_KIND_DOOM) {
        bochs_disable();
        vga_set_mode(VGA_MODE_TEXT);
        terminal_set_force_direct(0);
        terminal_clear();
    }

    /* A script window's "task" is whatever task called wm_script_open()
     * (no dedicated task is spawned for it — see wm_script_open()) — that
     * could be the same task running, say, the GUI terminal underneath
     * it. task_kill()-ing it here would kill that script's own task out
     * from under it, not just close the window. */
    if (w->kind == WIN_KIND_NATIVE) wm_gfx_native_closed(w);
    else if (w->kind != WIN_KIND_SCRIPT && w->kind != WIN_KIND_PANEL) task_kill((uint32_t)w->pid);
    w->open = 0;
    w->minimized = 0;
    w->maximized = 0;
    if (wm_focused == w) wm_focused = NULL;
    sound_close();
}

/* Lets a script (MicroPython's tosgui module, or anything similar added
 * later) turn its own calling task into the owner of a new window,
 * instead of spawning a dedicated task the way every other app does —
 * the script drives its own window directly via terminal_*()/wm_get_*()
 * calls, which already route through whichever task is "current"
 * (task_get_userdata()). Returns the previous userdata via
 * *out_prev_userdata so the caller can restore it on close (e.g. the
 * GUI terminal window it was running inside of). Returns NULL on
 * failure (no free window slot). */
void *wm_script_open(const char *title, void **out_prev_userdata)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return 0;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_SCRIPT;
    w->initial_cmd[0] = 0;
    strncpy(w->title, title, sizeof(w->title) - 1);
    w->title[sizeof(w->title) - 1] = 0;

    window_geom_init(w);

    if (out_prev_userdata) *out_prev_userdata = task_get_userdata();
    w->pid = (int)task_get_pid();
    task_set_userdata(w);
    wm_focused = w;
    return w;
}

void wm_script_close(void *handle, void *prev_userdata)
{
    window_t *w = (window_t *)handle;
    if (w) {
        w->open = 0;
        w->minimized = 0;
        w->maximized = 0;
        if (wm_focused == w) wm_focused = NULL;
    }
    task_set_userdata(prev_userdata);
}

/* Killing a window's task out from under it (e.g. from Task Manager)
 * needs the same cleanup wm_close_window() does — open/minimized/
 * maximized flags and wm_focused — not just task_kill(), or the now-dead
 * window keeps drawing its last frame forever. Returns 0 if pid matched
 * an open window (and it was closed), -1 if pid isn't a window task. */
int wm_kill_task_window(uint32_t pid)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].open && (uint32_t)windows[i].pid == pid) {
            wm_close_window(&windows[i]);
            return 0;
        }
    }
    return -1;
}

void wm_open_terminal(const char *cmd);
static void wm_open_window(const char *cmd) { wm_open_terminal(cmd); }

void wm_open_terminal(const char *cmd)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_TERMINAL;

    strncpy(w->initial_cmd, cmd, sizeof(w->initial_cmd) - 1);
    w->initial_cmd[sizeof(w->initial_cmd) - 1] = 0;

    if (cmd[0]) {
        const char *prefix = "Terminal - ";
        int k = 0;
        while (prefix[k] && k < (int)sizeof(w->title) - 1) { w->title[k] = prefix[k]; k++; }
        int j = 0;
        while (cmd[j] && k < (int)sizeof(w->title) - 1) { w->title[k++] = cmd[j++]; }
        w->title[k] = 0;
    } else {
        strcpy(w->title, "Terminal");
    }

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_notepad(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_NOTEPAD;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Notepad");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

void wm_open_notepad_file(const char *path)
{
    if (wm_gfx_mode && wm_gfx_open_native("notepad", path) == 0) return;
    notepad_open_path(path);
    wm_open_notepad();
}

static void wm_open_clock(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_CLOCK;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Clock");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_about(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_ABOUT;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "About This Computer");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_diskmgr(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_DISKMGR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Disk Utility");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_calculator(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 0;
    w->z = next_z++;
    w->kind = WIN_KIND_CALCULATOR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Calculator");

    window_geom_init(w);
    /* Scientific mode's extra two button rows need more vertical room
     * than the default non-maximized window size. */
    w->w0 = 42; w->h0 = 21;
    w->rest_w0 = w->w0; w->rest_h0 = w->h0;

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_filemgr(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_FILEMGR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Files");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_paint(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_PAINT;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Paint");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_viewer(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_VIEWER;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Image Viewer");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

void wm_open_viewer_file(const char *path)
{
    if (wm_gfx_mode && wm_gfx_open_native("viewer", path) == 0) return;
    viewer_open_path(path);
    wm_open_viewer();
}

static void wm_open_pdfviewer(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_PDFVIEWER;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "PDF Viewer");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

void wm_open_pdfviewer_file(const char *path)
{
    pdfview_open_path(path);
    wm_open_pdfviewer();
}

static void wm_open_stickynotes(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_STICKYNOTES;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Note Pad");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_doom(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_DOOM;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "DOOM");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_taskmgr(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_TASKMGR;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Task Manager");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_mediaplayer(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_MEDIAPLAYER;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Media Player");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_netmon(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_NETMON;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Network Monitor");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_snake(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_SNAKE;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "Snake");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

static void wm_open_2048(void)
{
    int slot = wm_find_free_slot();
    if (slot < 0) return;
    window_t *w = &windows[slot];
    terminal_surface_init(&w->surface);
    w->open = 1;
    w->minimized = 0;
    w->maximized = 1;
    w->z = next_z++;
    w->kind = WIN_KIND_2048;
    w->initial_cmd[0] = 0;
    strcpy(w->title, "2048");

    window_geom_init(w);

    pending_window = w;
    int pid = task_spawn(window_task_entry, w->title);
    if (pid < 0) { w->open = 0; return; }
    w->pid = pid;
    wm_focused = w;
}

/* Name-based app launcher for scripting APIs (T#, MicroPython) that
 * shouldn't need to know about every individual wm_open_*() function.
 * Returns 0 if the name was recognized (whether or not the window
 * actually opened — e.g. no free slot), -1 for an unknown name. */
int wm_open_app(const char *name)
{
    /* the graphical desktop has native versions of most apps */
    if (wm_gfx_mode && wm_gfx_open_native(name, 0) == 0) return 0;
    if (strcmp(name, "notepad") == 0) wm_open_notepad();
    else if (strcmp(name, "clock") == 0) wm_open_clock();
    else if (strcmp(name, "about") == 0) wm_open_about();
    else if (strcmp(name, "diskutil") == 0 || strcmp(name, "disk") == 0) wm_open_diskmgr();
    else if (strcmp(name, "calculator") == 0) wm_open_calculator();
    else if (strcmp(name, "files") == 0) wm_open_filemgr();
    else if (strcmp(name, "paint") == 0) wm_open_paint();
    else if (strcmp(name, "viewer") == 0) wm_open_viewer();
    else if (strcmp(name, "taskmgr") == 0) wm_open_taskmgr();
    else if (strcmp(name, "mediaplayer") == 0 || strcmp(name, "music") == 0) wm_open_mediaplayer();
    else if (strcmp(name, "netmon") == 0 || strcmp(name, "network") == 0) wm_open_netmon();
    else if (strcmp(name, "snake") == 0) wm_open_snake();
    else if (strcmp(name, "2048") == 0) wm_open_2048();
    else if (strcmp(name, "pdfviewer") == 0 || strcmp(name, "pdf") == 0) wm_open_pdfviewer();
    else if (strcmp(name, "notes") == 0 || strcmp(name, "stickynotes") == 0) wm_open_stickynotes();
    else if (strcmp(name, "doom") == 0) wm_open_doom();
    else if (strcmp(name, "terminal") == 0) wm_open_window("");
    else if (strcmp(name, "wm") == 0) { /* the window manager is what is running: nothing to start */ }
    else return -1;
    return 0;
}

static void draw_window(window_t *w)
{
    int x0 = w->x0, y0 = w->y0, w0 = w->w0, h0 = w->h0;

    int is_active = (w == wm_focused);
    uint8_t title_color = is_active ? mk_color(VGA_BLACK, VGA_WHITE) : mk_color(VGA_BLACK, VGA_LIGHT_GREY);

    vga_fill_rect(x0, y0, w0, 1, ' ', title_color);

    /* Mac-style pinstripes filling the active title bar */
    if (is_active) {
        for (int x = x0; x < x0 + w0; x += 2)
            vga_put(x, y0, '-', mk_color(VGA_LIGHT_GREY, VGA_WHITE));
    }

    /* close box on the left, like classic Mac OS */
    vga_put(x0 + 1, y0, ' ', mk_color(VGA_BLACK, VGA_WHITE));
    vga_fill_rect(x0 + 1, y0, 1, 1, 0xFE, title_color);

    int title_len = (int)strlen(w->title);
    int title_x = x0 + (w0 - title_len) / 2;
    if (title_x < x0 + 3) title_x = x0 + 3;
    vga_fill_rect(title_x - 1, y0, title_len + 2, 1, ' ', title_color);
    vga_text(title_x, y0, w->title, title_color);

    int gx = x0 + w0 - 4;
    vga_put(gx, y0, '_', title_color);
    vga_put(gx + 1, y0, 0xFE, title_color);
    vga_put(gx + 2, y0, 'X', mk_color(VGA_WHITE, VGA_RED));

    int content_h = h0 - 1;
    int vo = w->surface.view_offset;
    /* While the surface hasn't filled the window yet (cur_row hasn't
     * reached content_h - 1), content must stay top-anchored like any
     * ordinary terminal — line 1 at the top, growing downward — not
     * pinned to the window's bottom edge. Only once cur_row overflows
     * past content_h - 1 does the *overflow* amount start pushing
     * content up/scrolling, which is what naturally produces the
     * bottom-anchored behavior once the window is full and beyond
     * (including once cur_row itself is pinned at TERM_SURFACE_H - 1
     * by surface_scroll()). "overflow" unifies both cases: 0 while
     * there's nothing to scroll, growing by 1 per line afterward. */
    int newest_row = w->surface.cur_row;
    if (newest_row >= TERM_SURFACE_H) newest_row = TERM_SURFACE_H - 1;
    int overflow = newest_row - (content_h - 1);
    if (overflow < 0) overflow = 0;
    for (int row = 0; row < content_h; row++) {
        int cells_idx = overflow + row - vo;
        for (int col = 0; col < w0; col++) {
            uint16_t cell;
            if (cells_idx >= 0 && cells_idx < TERM_SURFACE_H && col < TERM_SURFACE_W) {
                cell = w->surface.cells[cells_idx * TERM_SURFACE_W + col];
            } else if (cells_idx < 0 && col < TERM_SURFACE_W) {
                const uint16_t *sb = terminal_scrollback_row(&w->surface, -cells_idx);
                cell = sb ? sb[col] : mk_cell(' ', mk_color(VGA_LIGHT_GREY, VGA_BLACK));
            } else {
                cell = mk_cell(' ', mk_color(VGA_LIGHT_GREY, VGA_BLACK));
            }
            int vx = x0 + col, vy = y0 + 1 + row;
            if (vx >= 0 && vx < WM_SCREEN_W && vy >= 0 && vy < WM_SCREEN_H - 1)
                backbuffer[vy * WM_SCREEN_W + vx] = cell;
        }
    }

    /* Scrollback slider in the rightmost content column, terminal windows
     * only — other app kinds (e.g. Notepad) manage that column themselves. */
    if (w->kind == WIN_KIND_TERMINAL && term_max_view_offset(w) > 0) {
        int max_off = term_max_view_offset(w);
        int thumb_row = content_h - 1 - (content_h > 1 ? ((content_h - 1) * vo) / max_off : 0);
        for (int row = 0; row < content_h; row++) {
            int vx = x0 + w0 - 1, vy = y0 + 1 + row;
            if (vx < 0 || vx >= WM_SCREEN_W || vy < 0 || vy >= WM_SCREEN_H - 1) continue;
            if (row == thumb_row)
                backbuffer[vy * WM_SCREEN_W + vx] = mk_cell(0xDB, mk_color(VGA_WHITE, VGA_BLUE));
            else
                backbuffer[vy * WM_SCREEN_W + vx] = mk_cell(0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));
        }
    }

    /* Resize-handle hint at the bottom-right corner of any non-maximized
     * window; dragging it is handled in wm_desktop_tick(). */
    if (!w->maximized) {
        int rx = x0 + w0 - 1, ry = y0 + h0 - 1;
        if (rx >= 0 && rx < WM_SCREEN_W && ry >= 0 && ry < WM_SCREEN_H - 1)
            backbuffer[ry * WM_SCREEN_W + rx] = mk_cell(0xD8, mk_color(VGA_DARK_GREY, VGA_LIGHT_GREY));
    }

    w->x0 = x0; w->y0 = y0; w->w0 = w0; w->h0 = h0;
}

static int dropdown_hit(int cx, int cy, int *out_index)
{
    for (int i = 0; i < menu_count; i++) {
        if (menu_disabled[i]) continue;
        if (cx >= menu_x0[i] && cx <= menu_x1[i] && cy == menu_y0[i]) { *out_index = i; return 1; }
    }
    return 0;
}

static void build_menu_items(int which)
{
    menu_count = 0;
    if (which == MENU_T) {
        menu_names[menu_count] = "About This Computer..."; menu_is_app[menu_count] = 4; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Notepad"; menu_is_app[menu_count] = 1; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Files"; menu_is_app[menu_count] = 7; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Paint"; menu_is_app[menu_count] = 8; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Image Viewer"; menu_is_app[menu_count] = 9; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Task Manager"; menu_is_app[menu_count] = 10; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Media Player"; menu_is_app[menu_count] = 11; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Network Monitor"; menu_is_app[menu_count] = 12; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Snake"; menu_is_app[menu_count] = 13; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "2048"; menu_is_app[menu_count] = 14; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "PDF Viewer"; menu_is_app[menu_count] = 15; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Notes"; menu_is_app[menu_count] = 16; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "DOOM"; menu_is_app[menu_count] = 17; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Calculator"; menu_is_app[menu_count] = 6; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Terminal"; menu_is_app[menu_count] = 3; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Clock"; menu_is_app[menu_count] = 2; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Disk Utility"; menu_is_app[menu_count] = 5; menu_disabled[menu_count] = 0;
        menu_count++;
        menu_names[menu_count] = "Shut Down"; menu_is_app[menu_count] = -2; menu_disabled[menu_count] = 0;
        menu_count++;
    } else if (which == MENU_FILE) {
        if (wm_focused && wm_focused->kind == WIN_KIND_NOTEPAD) {
            menu_names[menu_count] = "New"; menu_is_app[menu_count] = -5; menu_disabled[menu_count] = 0; menu_count++;
            menu_names[menu_count] = "Open..."; menu_is_app[menu_count] = -6; menu_disabled[menu_count] = 0; menu_count++;
            menu_names[menu_count] = "Save"; menu_is_app[menu_count] = -7; menu_disabled[menu_count] = 0; menu_count++;
        }
        menu_names[menu_count] = "New Window"; menu_is_app[menu_count] = -3; menu_disabled[menu_count] = 0; menu_count++;
        menu_names[menu_count] = "Close Window"; menu_is_app[menu_count] = -4; menu_disabled[menu_count] = 0; menu_count++;
    } else {
        menu_names[menu_count] = "(no items)"; menu_is_app[menu_count] = -1; menu_disabled[menu_count] = 1; menu_count++;
    }
}

static void draw_divider(int x0, int y, int w)
{
    vga_fill_rect(x0, y, w, 1, 0xC4, mk_color(VGA_LIGHT_GREY, VGA_WHITE));
}

/* The T menu (apple-menu equivalent) lists every shell command plus Notepad,
   far too many to fit one-per-row in 25 text rows, so the app list is laid
   out as a grid (like the original Start menu) while the header and
   Shut Down stay as full-width rows above/below it. */
static void draw_t_menu(int anchor_x0)
{
    build_menu_items(MENU_T);

    int header_idx = 0;
    int shutdown_idx = menu_count - 1;
    int apps_start = 1;
    int apps_count = menu_count - 2;

    int cols = 4;
    int item_w = 0;
    for (int i = 0; i < apps_count; i++) {
        int len = (int)strlen(menu_names[apps_start + i]) + 2;
        if (len > item_w) item_w = len;
    }
    int rows = (apps_count + cols - 1) / cols;
    int grid_w = cols * item_w;

    int header_w = (int)strlen(menu_names[header_idx]) + 2;
    int shutdown_w = (int)strlen(menu_names[shutdown_idx]) + 2;
    int total_w = grid_w;
    if (header_w > total_w) total_w = header_w;
    if (shutdown_w > total_w) total_w = shutdown_w;

    int mx0 = anchor_x0;
    if (mx0 + total_w >= WM_SCREEN_W) mx0 = WM_SCREEN_W - total_w;
    if (mx0 < 0) mx0 = 0;
    int my0 = MENU_ROW + 1;
    int menu_h = 1 + 1 + rows + 1 + 1;
    if (my0 + menu_h > DOCK_ROW) menu_h = DOCK_ROW - my0;

    vga_fill_rect(mx0, my0, total_w, menu_h, ' ', mk_color(VGA_BLACK, VGA_WHITE));
    for (int y = my0; y < my0 + menu_h; y++) vga_put(mx0 + total_w, y, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));
    vga_fill_rect(mx0, my0 + menu_h, total_w + 1, 1, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));

    uint8_t header_color = menu_disabled[header_idx] ? mk_color(VGA_LIGHT_GREY, VGA_WHITE) : mk_color(VGA_BLACK, VGA_WHITE);
    vga_text(mx0 + 1, my0, menu_names[header_idx], header_color);
    menu_x0[header_idx] = mx0; menu_y0[header_idx] = my0; menu_x1[header_idx] = mx0 + total_w - 1;

    draw_divider(mx0, my0 + 1, total_w);

    for (int i = 0; i < apps_count; i++) {
        int idx = apps_start + i;
        int col = i % cols;
        int row = i / cols;
        int ix = mx0 + 1 + col * item_w;
        int iy = my0 + 2 + row;
        if (iy >= my0 + menu_h - 2) continue;
        vga_text(ix, iy, menu_names[idx], mk_color(VGA_BLACK, VGA_WHITE));
        menu_x0[idx] = ix; menu_y0[idx] = iy; menu_x1[idx] = ix + item_w - 1;
    }

    int shutdown_y = my0 + 2 + rows + 1;
    draw_divider(mx0, shutdown_y - 1, total_w);
    vga_text(mx0 + 1, shutdown_y, menu_names[shutdown_idx], mk_color(VGA_BLACK, VGA_WHITE));
    menu_x0[shutdown_idx] = mx0; menu_y0[shutdown_idx] = shutdown_y; menu_x1[shutdown_idx] = mx0 + total_w - 1;
}

static void draw_simple_menu(int which, int anchor_x0)
{
    build_menu_items(which);

    int item_w = 0;
    for (int i = 0; i < menu_count; i++) {
        int len = (int)strlen(menu_names[i]) + 2;
        if (len > item_w) item_w = len;
    }
    if (item_w < 14) item_w = 14;

    int mx0 = anchor_x0;
    if (mx0 + item_w >= WM_SCREEN_W) mx0 = WM_SCREEN_W - item_w;
    if (mx0 < 0) mx0 = 0;
    int my0 = MENU_ROW + 1;
    int menu_h = menu_count + 1;

    vga_fill_rect(mx0, my0, item_w, menu_h, ' ', mk_color(VGA_BLACK, VGA_WHITE));
    for (int y = my0; y < my0 + menu_h; y++) vga_put(mx0 + item_w, y, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));
    vga_fill_rect(mx0, my0 + menu_h, item_w + 1, 1, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));

    for (int i = 0; i < menu_count; i++) {
        int ix = mx0 + 1;
        int iy = my0 + i;
        uint8_t c = menu_disabled[i] ? mk_color(VGA_LIGHT_GREY, VGA_WHITE) : mk_color(VGA_BLACK, VGA_WHITE);
        vga_text(ix, iy, menu_names[i], c);
        menu_x0[i] = mx0; menu_y0[i] = iy; menu_x1[i] = mx0 + item_w - 1;
    }
}

static void draw_dropdown_menu(int which, int anchor_x0, int anchor_x1)
{
    (void)anchor_x1;
    if (which == MENU_T) draw_t_menu(anchor_x0);
    else draw_simple_menu(which, anchor_x0);
}

static void draw_menu_bar(void)
{
    vga_fill_rect(0, MENU_ROW, WM_SCREEN_W, 1, ' ', mk_color(VGA_BLACK, VGA_WHITE));

    t_x0 = 1; t_x1 = 2;
    vga_put(t_x0, MENU_ROW, 'T', mk_color(VGA_RED, VGA_WHITE));

    int x = 4;
    uint8_t menu_color = mk_color(VGA_BLACK, VGA_WHITE);
    uint8_t menu_active_color = mk_color(VGA_WHITE, VGA_BLACK);

    const char *labels[5] = { "File", "Edit", "View", "Label", "Special" };
    int active_ids[5] = { MENU_FILE, MENU_EDIT, MENU_VIEW, MENU_LABEL, MENU_SPECIAL };
    int *xs0[5] = { &file_x0, &edit_x0, &view_x0, &label_x0, &special_x0 };
    int *xs1[5] = { &file_x1, &edit_x1, &view_x1, &label_x1, &special_x1 };

    for (int i = 0; i < 5; i++) {
        int len = (int)strlen(labels[i]);
        uint8_t c = (active_menu == active_ids[i]) ? menu_active_color : menu_color;
        if (active_menu == active_ids[i]) vga_fill_rect(x - 1, MENU_ROW, len + 2, 1, ' ', c);
        vga_text(x, MENU_ROW, labels[i], c);
        *xs0[i] = x - 1; *xs1[i] = x + len;
        x += len + 3;
    }

    if (active_menu == MENU_T) vga_fill_rect(t_x0 - 1, MENU_ROW, 4, 1, ' ', menu_active_color);

    cmos_time_t t;
    cmos_get_time(&t);

    char netbuf[24];
    if (net_ip == 0) {
        strcpy(netbuf, "NET: --");
    } else {
        int k = 0;
        const char *pfx = "NET: ";
        while (pfx[k]) { netbuf[k] = pfx[k]; k++; }
        unsigned int o1 = net_ip & 0xFF;
        unsigned int o2 = (net_ip >> 8) & 0xFF;
        unsigned int o3 = (net_ip >> 16) & 0xFF;
        unsigned int o4 = (net_ip >> 24) & 0xFF;
        k += fmt_uint(netbuf + k, o1); netbuf[k++] = '.';
        k += fmt_uint(netbuf + k, o2); netbuf[k++] = '.';
        k += fmt_uint(netbuf + k, o3); netbuf[k++] = '.';
        k += fmt_uint(netbuf + k, o4);
        netbuf[k] = 0;
    }

    char right[40];
    int rk = 0;
    int nk = 0;
    while (netbuf[nk]) right[rk++] = netbuf[nk++];
    right[rk++] = ' '; right[rk++] = ' '; right[rk++] = 0xF8; right[rk++] = ' ';
    rk += fmt_uint02(right + rk, (unsigned int)t.hour);
    right[rk++] = ':';
    rk += fmt_uint02(right + rk, (unsigned int)t.minute);
    right[rk] = 0;

    int rlen = (int)strlen(right);
    vga_text(WM_SCREEN_W - rlen - 1, MENU_ROW, right, mk_color(VGA_BLACK, VGA_WHITE));
}

static const char *get_icon_char(int kind)
{
    switch (kind) {
        case WIN_KIND_TERMINAL: return ">";
        case WIN_KIND_NOTEPAD:  return "\xF8";
        case WIN_KIND_CLOCK:    return "\xF8";
        case WIN_KIND_ABOUT:    return "?";
        case WIN_KIND_DISKMGR:  return "\xF7";
        case WIN_KIND_CALCULATOR: return "#";
        case WIN_KIND_FILEMGR:  return "\xF6";
        case WIN_KIND_PAINT:    return "\x0E";
        case WIN_KIND_VIEWER:   return "\x0C";
        case WIN_KIND_TASKMGR:  return "\x05";
        case WIN_KIND_SCRIPT:      return "\x1E";
        case WIN_KIND_MEDIAPLAYER: return "\x0D"; /* ♪ */
        case WIN_KIND_NETMON:      return "\x1E"; /* ▲ (network up-arrow) */
        case WIN_KIND_SNAKE:       return "\x06"; /* ♠-ish blob, stands in for a snake glyph */
        case WIN_KIND_2048:        return "2";
        case WIN_KIND_PDFVIEWER:   return "\x0F"; /* page-ish glyph */
        case WIN_KIND_STICKYNOTES: return "\xB1"; /* dog-ear-ish shade block */
        case WIN_KIND_DOOM:        return "\x02"; /* skull-ish smiley glyph */
        default:                return ".";
    }
}

static void draw_dock(void)
{
    vga_fill_rect(0, DOCK_ROW, WM_SCREEN_W, 1, ' ', mk_color(VGA_BLACK, VGA_LIGHT_GREY));

    int x = 1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->open) continue;
        char buf[14];
        int k = 0;
        buf[k++] = ' ';
        const char *icon = get_icon_char(w->kind);
        while (*icon) buf[k++] = *icon++;
        buf[k++] = ' ';
        int j = 0;
        while (w->title[j] && k < 12) { buf[k++] = w->title[j++]; }
        buf[k] = 0;
        int len = k + 2;
        if (x + len >= WM_SCREEN_W - 8) break;
        uint8_t c = (w == wm_focused && !w->minimized) ? mk_color(VGA_WHITE, VGA_BLUE) : mk_color(VGA_BLACK, VGA_LIGHT_GREY);
        uint8_t icon_c = (w == wm_focused && !w->minimized) ? mk_color(VGA_LIGHT_CYAN, VGA_BLUE) : mk_color(VGA_DARK_GREY, VGA_LIGHT_GREY);
        vga_put(x, DOCK_ROW, '[', c);
        vga_put(x + 1, DOCK_ROW, buf[0], icon_c);
        vga_put(x + 2, DOCK_ROW, buf[1], icon_c);
        vga_put(x + 3, DOCK_ROW, ' ', c);
        vga_text(x + 4, DOCK_ROW, buf + 3, c);
        vga_put(x + len - 1, DOCK_ROW, ']', c);
        w->tb_x0 = x; w->tb_x1 = x + len - 1;
        x += len + 1;
    }
}

static void draw_trash(void)
{
    int x = WM_SCREEN_W - 7;
    int y = DOCK_ROW - 6;
    uint8_t line = mk_color(VGA_BLACK, VGA_WHITE);

    vga_put(x + 2, y, 0xD5, line);
    vga_put(x + 3, y, 0xC4, line);
    vga_put(x + 4, y, 0xB8, line);

    vga_put(x + 1, y + 1, 0xC4, line);
    vga_put(x + 2, y + 1, 0xD1, line);
    vga_put(x + 3, y + 1, 0xC4, line);
    vga_put(x + 4, y + 1, 0xD1, line);
    vga_put(x + 5, y + 1, 0xC4, line);

    vga_put(x + 1, y + 2, 0xB3, line);
    vga_put(x + 2, y + 2, ' ', line);
    vga_put(x + 3, y + 2, 0xB3, line);
    vga_put(x + 4, y + 2, ' ', line);
    vga_put(x + 5, y + 2, 0xB3, line);

    vga_put(x + 1, y + 3, 0xB3, line);
    vga_put(x + 2, y + 3, ' ', line);
    vga_put(x + 3, y + 3, 0xB3, line);
    vga_put(x + 4, y + 3, ' ', line);
    vga_put(x + 5, y + 3, 0xB3, line);

    vga_put(x, y + 4, 0xC0, line);
    vga_fill_rect(x + 1, y + 4, 5, 1, 0xC4, line);
    vga_put(x + 6, y + 4, 0xD9, line);

    vga_text(x, y + 5, " Trash ", mk_color(VGA_BLACK, VGA_WHITE));
}

static void draw_cursor(void)
{
    int mx, my; uint8_t btns;
    mouse_get_state(&mx, &my, &btns);
    (void)btns;
    if (mx < 0 || mx >= WM_SCREEN_W || my < 0 || my >= WM_SCREEN_H) return;
    uint16_t cell = backbuffer[my * WM_SCREEN_W + mx];
    uint8_t bg = (cell >> 12) & 0x0F;
    uint8_t cursor_fg = (bg == VGA_BLACK) ? VGA_WHITE : VGA_BLACK;
    backbuffer[my * WM_SCREEN_W + mx] = mk_cell(0x10, mk_color(cursor_fg, bg));
}

#define FRAME_INTERVAL_TICKS 1
static uint32_t last_draw_tick = 0;

static void wm_shutdown(void)
{
    uint8_t color = mk_color(VGA_WHITE, VGA_BLACK);
    for (int y = 0; y < WM_SCREEN_H; y++)
        for (int x = 0; x < WM_SCREEN_W; x++)
            backbuffer[y * WM_SCREEN_W + x] = mk_cell(' ', color);

    const char *lines[] = {
        "tOS",
        "",
        "It is now safe to turn off your computer.",
    };
    int start_y = WM_SCREEN_H / 2 - 1;
    for (int i = 0; i < 3; i++) {
        const char *s = lines[i];
        int len = (int)strlen(s);
        int x = (WM_SCREEN_W - len) / 2;
        vga_text(x, start_y + i, s, color);
    }

    for (int i = 0; i < WM_SCREEN_W * WM_SCREEN_H; i++) VGA_MEM[i] = backbuffer[i];

    for (;;) asm volatile("hlt");
}

static void handle_menu_click(int idx)
{
    sound_click();
    if (active_menu == MENU_T) {
        if (menu_is_app[idx] == -2) {
            wm_shutdown();
        } else if (menu_is_app[idx] == 1) {
            wm_open_notepad();
        } else if (menu_is_app[idx] == 2) {
            wm_open_clock();
        } else if (menu_is_app[idx] == 3) {
            wm_open_window("");
        } else if (menu_is_app[idx] == 4) {
            wm_open_about();
        } else if (menu_is_app[idx] == 5) {
            wm_open_diskmgr();
        } else if (menu_is_app[idx] == 6) {
            wm_open_calculator();
        } else if (menu_is_app[idx] == 7) {
            wm_open_filemgr();
        } else if (menu_is_app[idx] == 8) {
            wm_open_paint();
        } else if (menu_is_app[idx] == 9) {
            wm_open_viewer();
        } else if (menu_is_app[idx] == 10) {
            wm_open_taskmgr();
        } else if (menu_is_app[idx] == 11) {
            wm_open_mediaplayer();
        } else if (menu_is_app[idx] == 12) {
            wm_open_netmon();
        } else if (menu_is_app[idx] == 13) {
            wm_open_snake();
        } else if (menu_is_app[idx] == 14) {
            wm_open_2048();
        } else if (menu_is_app[idx] == 15) {
            wm_open_pdfviewer();
        } else if (menu_is_app[idx] == 16) {
            wm_open_stickynotes();
        } else if (menu_is_app[idx] == 17) {
            wm_open_doom();
        }
    } else if (active_menu == MENU_FILE) {
        if (menu_is_app[idx] == -3) {
            wm_open_window("");
        } else if (menu_is_app[idx] == -4) {
            if (wm_focused) wm_close_window(wm_focused);
        } else if (menu_is_app[idx] == -5) {
            if (wm_focused) { action_target = wm_focused; pending_action = WM_ACTION_NEW; }
        } else if (menu_is_app[idx] == -6) {
            if (wm_focused) { action_target = wm_focused; pending_action = WM_ACTION_OPEN; }
        } else if (menu_is_app[idx] == -7) {
            if (wm_focused) { action_target = wm_focused; pending_action = WM_ACTION_SAVE; }
        }
    }
}

/* Builds "<base> (1)<suffix>", "<base> (2)<suffix>", etc. into out until
 * an unused name is found (capped at 50 tries as a safety net). */
static void unique_name(const char *base, const char *suffix, char *out, int out_cap)
{
    (void)out_cap;
    strcpy(out, base);
    strcat(out, suffix);

    int n = 0;
    while (fsbridge_exists(out) && n < 50) {
        n++;
        char buf[48];
        int k = 0;
        const char *p = base;
        while (*p && k < (int)sizeof(buf) - 1) buf[k++] = *p++;
        p = " (";
        while (*p && k < (int)sizeof(buf) - 1) buf[k++] = *p++;
        char tmp[12];
        int tn = 0, v = n;
        while (v > 0) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        while (tn > 0 && k < (int)sizeof(buf) - 1) buf[k++] = tmp[--tn];
        if (k < (int)sizeof(buf) - 1) buf[k++] = ')';
        p = suffix;
        while (*p && k < (int)sizeof(buf) - 1) buf[k++] = *p++;
        buf[k] = 0;
        strncpy(out, buf, out_cap - 1);
        out[out_cap - 1] = 0;
    }
}

void wm_desktop_new_folder(void)
{
    char name[48];
    unique_name("New Folder", "", name, sizeof(name));
    fsbridge_mkdir(name);
    wm_open_filemgr();
}

void wm_desktop_new_file(void)
{
    char name[48];
    unique_name("New File", ".txt", name, sizeof(name));
    fsbridge_create(name);
    wm_open_notepad_file(name);
}

static void desktop_change_background(void)
{
    if (wallpaper_active) { wallpaper_active = 0; return; }
    desktop_bg_idx = (desktop_bg_idx + 1) % DESKTOP_BG_COUNT;
}

static void draw_context_menu(void)
{
    int item_w = 0;
    for (int i = 0; i < CTX_NUM_ITEMS; i++) {
        int len = (int)strlen(ctx_labels[i]) + 2;
        if (len > item_w) item_w = len;
    }
    ctx_w = item_w;

    int mx0 = ctx_x0;
    if (mx0 + item_w >= WM_SCREEN_W) mx0 = WM_SCREEN_W - item_w;
    if (mx0 < 0) mx0 = 0;
    int menu_h = CTX_NUM_ITEMS + 1;
    int my0 = ctx_y0;
    if (my0 + menu_h > DOCK_ROW) my0 = DOCK_ROW - menu_h;
    if (my0 < MENU_ROW + 1) my0 = MENU_ROW + 1;
    ctx_x0 = mx0;
    ctx_y0 = my0;

    vga_fill_rect(mx0, my0, item_w, menu_h, ' ', mk_color(VGA_BLACK, VGA_WHITE));
    for (int y = my0; y < my0 + menu_h; y++) vga_put(mx0 + item_w, y, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));
    vga_fill_rect(mx0, my0 + menu_h, item_w + 1, 1, 0xB1, mk_color(VGA_DARK_GREY, VGA_BLACK));

    for (int i = 0; i < CTX_NUM_ITEMS; i++) {
        int iy = my0 + i;
        vga_text(mx0 + 1, iy, ctx_labels[i], mk_color(VGA_BLACK, VGA_WHITE));
        ctx_item_y0[i] = iy;
    }
}

/* Returns 1 if the click landed on the menu itself (consumed), 0 if it
 * landed elsewhere — a click outside the menu should still close it,
 * but must fall through to its normal handling (menu bar, dock,
 * window, ...) instead of being silently swallowed, otherwise a
 * left-over open context menu eats the *next* unrelated click anywhere
 * on screen and makes it look like the menu re-opened somewhere
 * random or that a real click on the T/File/... bar did nothing. */
static int handle_context_click(int cx, int cy)
{
    ctx_open = 0;
    if (cx >= ctx_x0 && cx < ctx_x0 + ctx_w &&
        cy >= ctx_item_y0[0] && cy < ctx_item_y0[0] + CTX_NUM_ITEMS) {
        int idx = cy - ctx_item_y0[0];
        switch (idx) {
            case 0: wm_desktop_new_folder(); break;
            case 1: wm_desktop_new_file(); break;
            case 2: wm_open_filemgr(); break;
            case 3: wm_open_window(""); break;
            case 4: wm_open_about(); break;
            case 5: desktop_change_background(); break;
            case 6: break; /* "Refresh Desktop" — the desktop already redraws every frame */
        }
        return 1;
    }
    return 0;
}

static void wm_desktop_tick(void)
{
    /* Another task may be showing the `crash` fake-panic screen right
     * now (drawn straight to real VGA memory). Don't repaint the
     * desktop over it -- the desktop task keeps running and getting
     * preempted back in via the timer tick regardless of what the
     * other task is doing, so without this check it stomps the crash
     * screen on the very next tick, making it look like `crash` does
     * nothing. */
    if (crash_screen_is_active()) return;

    /* The graphical desktop has its own frame loop (wm_gfx.c); it deals with other programs
     * taking over the display itself, so it goes before the check below. */
    if (wm_gfx_mode) { wm_gfx_tick(); task_yield(); return; }

    /* Same reasoning as the crash-screen check above: some other task
     * has switched the display into real VBE pixel graphics (DOOM,
     * vgatest, fbtest, a SYS_GFX_INIT-based .t program, ...) and owns
     * it until it switches back. The desktop task still gets preempted
     * back in on every timer tick regardless, so without this it would
     * keep writing 0xB8000 the whole time -- harmless once the mode
     * switch itself is done, but still wrong, and each graphics-mode
     * caller doesn't need its own bespoke window kind (WIN_KIND_DOOM)
     * just to keep the desktop from fighting it. */
    if (bochs_graphics_active()) return;

    mouse_poll();

    if (drag_window) {
        int mx, my;
        uint8_t btns;
        mouse_get_state(&mx, &my, &btns);
        if (btns & 1) {
            int nx = mx - drag_offset_x;
            int ny = my - drag_offset_y;
            if (nx < 0) nx = 0;
            if (ny < MENU_ROW + 1) ny = MENU_ROW + 1;
            if (nx > WM_SCREEN_W - drag_window->w0) nx = WM_SCREEN_W - drag_window->w0;
            if (ny > DOCK_ROW - 1) ny = DOCK_ROW - 1;
            drag_window->x0 = nx;
            drag_window->y0 = ny;
        } else {
            drag_window = NULL;
        }
    }

    if (resize_window) {
        int mx, my;
        uint8_t btns;
        mouse_get_state(&mx, &my, &btns);
        if (btns & 1) {
            int neww = resize_start_w + (mx - resize_start_mx);
            int newh = resize_start_h + (my - resize_start_my);
            if (neww < MIN_WIN_W) neww = MIN_WIN_W;
            if (newh < MIN_WIN_H) newh = MIN_WIN_H;
            if (resize_window->x0 + neww > WM_SCREEN_W) neww = WM_SCREEN_W - resize_window->x0;
            if (resize_window->y0 + newh > DOCK_ROW) newh = DOCK_ROW - resize_window->y0;
            resize_window->w0 = neww;
            resize_window->h0 = newh;
        } else {
            resize_window = NULL;
        }
    }

    int wheel = mouse_get_wheel_delta();
    if (wheel != 0) {
        int wmx, wmy;
        uint8_t wbtns;
        mouse_get_state(&wmx, &wmy, &wbtns);
        (void)wbtns;

        window_t *under = NULL;
        int under_z = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t *w = &windows[i];
            if (!w->open || w->minimized) continue;
            if (wmx >= w->x0 && wmx < w->x0 + w->w0 && wmy > w->y0 && wmy < w->y0 + w->h0) {
                if (w->z > under_z) { under_z = w->z; under = w; }
            }
        }

        if (under) {
            if (under->kind == WIN_KIND_TERMINAL && term_max_view_offset(under) > 0) {
                int max_off = term_max_view_offset(under);
                int vo = under->surface.view_offset + wheel * 2;
                if (vo < 0) vo = 0;
                if (vo > max_off) vo = max_off;
                under->surface.view_offset = vo;
            } else {
                wheel_target = under;
                pending_wheel_delta = wheel;
            }
        }
    }

    int rcx, rcy;
    if (mouse_get_rclick(&rcx, &rcy) && !ctx_open && active_menu == MENU_NONE &&
        rcy != MENU_ROW && rcy != DOCK_ROW) {
        window_t *under = NULL;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            window_t *w = &windows[i];
            if (!w->open || w->minimized) continue;
            if (rcx >= w->x0 && rcx < w->x0 + w->w0 && rcy >= w->y0 && rcy < w->y0 + w->h0) { under = w; break; }
        }
        if (!under) {
            ctx_open = 1;
            ctx_x0 = rcx;
            ctx_y0 = rcy;
        }
    }

    int cx, cy;
    if (mouse_get_click(&cx, &cy)) {
        if (ctx_open && handle_context_click(cx, cy)) {
            /* click landed on the menu and was already handled */
        } else if (active_menu != MENU_NONE) {
            int idx;
            if (dropdown_hit(cx, cy, &idx)) {
                handle_menu_click(idx);
            }
            active_menu = MENU_NONE;
        } else if (cy == MENU_ROW) {
            if (cx >= t_x0 - 1 && cx <= t_x1) active_menu = MENU_T;
            else if (cx >= file_x0 && cx <= file_x1) active_menu = MENU_FILE;
            else if (cx >= edit_x0 && cx <= edit_x1) active_menu = MENU_EDIT;
            else if (cx >= view_x0 && cx <= view_x1) active_menu = MENU_VIEW;
            else if (cx >= label_x0 && cx <= label_x1) active_menu = MENU_LABEL;
            else if (cx >= special_x0 && cx <= special_x1) active_menu = MENU_SPECIAL;
        } else if (cy == DOCK_ROW) {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                window_t *w = &windows[i];
                if (w->open && cx >= w->tb_x0 && cx <= w->tb_x1) {
                    sound_click();
                    if (w == wm_focused && !w->minimized) {
                        w->minimized = 1;
                    } else {
                        wm_focus_window(w);
                    }
                    break;
                }
            }
        } else {
            window_t *hit = NULL;
            int best_z = -1;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                window_t *w = &windows[i];
                if (!w->open || w->minimized) continue;
                if (cx >= w->x0 && cx < w->x0 + w->w0 && cy >= w->y0 && cy < w->y0 + w->h0) {
                    if (w->z > best_z) { best_z = w->z; hit = w; }
                }
            }
            if (hit) {
                if (cy == hit->y0) {
                    int gx = hit->x0 + hit->w0 - 4;
                    if (cx == hit->x0 + 1) { wm_close_window(hit); }
                    else if (cx == gx) { hit->minimized = 1; }
                    else if (cx == gx + 1) {
                        if (!hit->maximized) {
                            hit->rest_x0 = hit->x0; hit->rest_y0 = hit->y0;
                            hit->rest_w0 = hit->w0; hit->rest_h0 = hit->h0;
                            hit->maximized = 1;
                            hit->x0 = 0; hit->y0 = MENU_ROW + 1; hit->w0 = WM_SCREEN_W; hit->h0 = WM_SCREEN_H - 2;
                        } else {
                            hit->maximized = 0;
                            hit->x0 = hit->rest_x0; hit->y0 = hit->rest_y0;
                            hit->w0 = hit->rest_w0; hit->h0 = hit->rest_h0;
                        }
                    }
                    else if (cx == gx + 2) { wm_close_window(hit); }
                    else {
                        wm_focus_window(hit);
                        if (!hit->maximized) {
                            drag_window = hit;
                            drag_offset_x = cx - hit->x0;
                            drag_offset_y = cy - hit->y0;
                        }
                    }
                } else if (!hit->maximized && cx == hit->x0 + hit->w0 - 1 && cy == hit->y0 + hit->h0 - 1) {
                    wm_focus_window(hit);
                    resize_window = hit;
                    resize_start_w = hit->w0;
                    resize_start_h = hit->h0;
                    resize_start_mx = cx;
                    resize_start_my = cy;
                } else if (hit->kind == WIN_KIND_TERMINAL && term_max_view_offset(hit) > 0 &&
                           cx == hit->x0 + hit->w0 - 1) {
                    wm_focus_window(hit);
                    int content_h = hit->h0 - 1;
                    int row_clicked = cy - hit->y0 - 1;
                    int max_off = term_max_view_offset(hit);
                    int vo = (content_h > 1)
                        ? max_off - (row_clicked * max_off) / (content_h - 1)
                        : 0;
                    if (vo < 0) vo = 0;
                    if (vo > max_off) vo = max_off;
                    hit->surface.view_offset = vo;
                } else {
                    int was_focused = (hit == wm_focused);
                    wm_focus_window(hit);
                    if (was_focused) {
                        content_click_target = hit;
                        content_click_x = cx - hit->x0;
                        content_click_y = cy - hit->y0 - 1;
                    }
                }
            }
        }
    }

    uint32_t now = task_get_ticks();
    if (now - last_draw_tick >= FRAME_INTERVAL_TICKS) {
        last_draw_tick = now;

        /* Classic Mac OS desktop: a flat background with a sparse stipple,
           instead of a solid teal fill. Background color is cycled via
           the desktop right-click menu's "Change Background". */
        {
            uint8_t bg = desktop_bg_palette[desktop_bg_idx];
            for (int y = MENU_ROW + 1; y < DOCK_ROW; y++) {
                for (int x = 0; x < WM_SCREEN_W; x++) {
                    if (wallpaper_active) {
                        vga_put(x, y, ' ', mk_color(VGA_DARK_GREY, wallpaper_color[y][x]));
                    } else if (((x + y * 3) % 7) == 0) {
                        vga_put(x, y, 0xFA, mk_color(VGA_DARK_GREY, bg));
                    } else {
                        vga_put(x, y, ' ', mk_color(VGA_DARK_GREY, bg));
                    }
                }
            }
        }

        draw_trash();

        int order[MAX_WINDOWS];
        int n = 0;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (windows[i].open && !windows[i].minimized) order[n++] = i;
        for (int i = 1; i < n; i++) {
            int key = order[i];
            int keyz = windows[key].z;
            int j = i - 1;
            while (j >= 0 && windows[order[j]].z > keyz) { order[j + 1] = order[j]; j--; }
            order[j + 1] = key;
        }
        for (int i = 0; i < n; i++) draw_window(&windows[order[i]]);

        draw_dock();
        draw_menu_bar();

        if (active_menu != MENU_NONE) {
            int ax0, ax1;
            switch (active_menu) {
            case MENU_T:       ax0 = t_x0 - 1;   ax1 = t_x1;     break;
            case MENU_FILE:    ax0 = file_x0;    ax1 = file_x1;  break;
            case MENU_EDIT:    ax0 = edit_x0;    ax1 = edit_x1;  break;
            case MENU_VIEW:    ax0 = view_x0;    ax1 = view_x1;  break;
            case MENU_LABEL:   ax0 = label_x0;   ax1 = label_x1; break;
            default:           ax0 = special_x0; ax1 = special_x1; break;
            }
            draw_dropdown_menu(active_menu, ax0, ax1);
        }

        if (ctx_open) draw_context_menu();

        draw_cursor();

        for (int i = 0; i < WM_SCREEN_W * WM_SCREEN_H; i++) VGA_MEM[i] = backbuffer[i];
    }

    task_yield();
}

void wm_poll(void)
{
    if (task_get_userdata()) return;
    wm_desktop_tick();
}

void wm_init(void)
{
    mouse_init();
    /* a pixel desktop whenever there is a framebuffer to draw it on; the text desktop otherwise */
    wm_gfx_mode = (wm_gfx_init() == 0);
    for (int i = 0; i < MAX_WINDOWS; i++) windows[i].open = 0;
    wm_focused = NULL;
    active_menu = MENU_NONE;
    next_z = 1;
}

void wm_run(void)
{
    for (;;) wm_poll();
}
