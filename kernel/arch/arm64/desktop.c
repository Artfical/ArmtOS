#include <stdint.h>
#include "bcm.h"
#include "uart.h"
#include "irq.h"
#include "heap.h"
#include "fb.h"
#include "fat.h"
#include "emmc.h"
#include "loader.h"
#include "mailbox.h"
#include "dwc2.h"
#include "usbhid.h"
#include "hwapi.h"
#include "cfgfile.h"
#include "version.h"
#include "string_arm.h"
#include "compat/cmos.h"
#include "../../display/gfx.h"

/*
 * The ArmtOS desktop.
 *
 * The same look as tOS's graphical desktop -- frosted top bar and dock, windows with soft
 * shadows and rounded corners, an app launcher -- drawn into the firmware's framebuffer
 * (1280x720 unless config.txt or Settings say otherwise), driven by a USB mouse and keyboard.
 *
 * There is no scheduler on the board, so the desktop runs whenever the shell is waiting for a
 * key (shell_readline calls desktop_tick), and the Terminal window *is* the shell: everything
 * the shell prints is mirrored into it, and what is typed into it goes to the shell. The other
 * windows are built-in panels for the Pi itself: system information, Settings (display and the
 * raspi-config interface switches), the GPIO header, the network and the files on the card.
 */

int  netcmd_up(void);
int  netcmd_is_up(void);
int  net_command(int argc, char **argv);
extern uint32_t net_ip, net_gateway, net_dns;
extern uint8_t net_mac[6];
extern char nic_driver_name[32];

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
#define CPAD_X      10
#define CPAD_Y      6

#define C_TEXT      0xF2F3F5
#define C_DIM       0xA1A4AE
#define C_ACCENT    0x3B82F6
#define C_PANEL     0x1E1F25
#define C_BUTTON    0x3A3C46

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
static int wallpaper_choice;
static int reboot_needed;
static char status_line[96];

extern void (*uart_mirror)(char c);
static void (*uart_mirror_get(void))(char) { return uart_mirror; }
static void uart_mirror_set(void (*m)(char)) { uart_mirror = m; }

static uint32_t now_ms(void) { return (uint32_t)(timer_us() / 1000); }

/* ------------------------------------------------------------------ the terminal surface */

#define TCOLS 80
#define TROWS 24
#define TSCROLL 300
static uint8_t tcells[TROWS][TCOLS];
static uint8_t tscroll[TSCROLL][TCOLS];
static int tscroll_head, tscroll_count, tview;
static int trow, tcol;
static int term_dirty;

static void term_newline(void)
{
    tcol = 0;
    if (trow < TROWS - 1) { trow++; return; }
    memcpy(tscroll[tscroll_head], tcells[0], TCOLS);
    tscroll_head = (tscroll_head + 1) % TSCROLL;
    if (tscroll_count < TSCROLL) tscroll_count++;
    memmove(tcells[0], tcells[1], (TROWS - 1) * TCOLS);
    memset(tcells[TROWS - 1], ' ', TCOLS);
}

static void redraw_soon(void);

/* every character the shell prints arrives here as well as on the serial line */
static void term_putc(char c)
{
    if (c == '\r') { tcol = 0; }
    else if (c == '\n') term_newline();
    else if (c == '\b') { if (tcol > 0) tcol--; }
    else if (c == '\t') { do { tcells[trow][tcol++] = ' '; } while (tcol % 8 && tcol < TCOLS); }
    else if ((unsigned char)c >= 32) {
        if (tcol >= TCOLS) term_newline();
        tcells[trow][tcol++] = (uint8_t)c;
    }
    tview = 0;
    term_dirty = 1;
    redraw_soon();
}

/* ------------------------------------------------------------------ windows */

enum { APP_TERMINAL, APP_SYSTEM, APP_SETTINGS, APP_GPIO, APP_NETWORK, APP_FILES, APP_COUNT };

typedef struct { const char *title, *icon; int w, h; } app_t;
static const app_t apps[APP_COUNT] = {
    { "Terminal", "terminal", TCOLS * CW + 2 * CPAD_X, TITLE_H + TROWS * CH + 2 * CPAD_Y },
    { "System",   "about",    460, 420 },
    { "Settings", "settings", 600, 470 },
    { "GPIO",     "gpio",     560, 520 },
    { "Network",  "netmon",   480, 380 },
    { "Files",    "files",    480, 420 },
};

typedef struct { int open, minimized, x, y, w, h, z; } win_t;
static win_t wins[APP_COUNT];
static int next_z = 1, focused = -1;
static int drag = -1, drag_dx, drag_dy;

static void focus(int a)
{
    wins[a].minimized = 0;
    wins[a].z = next_z++;
    focused = a;
    dirty = 1;
}

static void open_app(int a)
{
    if (!wins[a].open) {
        int n = 0;
        for (int i = 0; i < APP_COUNT; i++) if (wins[i].open) n++;
        wins[a].open = 1;
        wins[a].w = apps[a].w;
        wins[a].h = apps[a].h;
        wins[a].x = (SW - wins[a].w) / 2 - 60 + n * 30;
        wins[a].y = TOPBAR_H + (SH - TOPBAR_H - DOCK_H - wins[a].h) / 2 - 20 + n * 26;
        if (wins[a].y < TOPBAR_H + 6) wins[a].y = TOPBAR_H + 6;
        if (wins[a].x < 6) wins[a].x = 6;
    }
    focus(a);
}

static void close_app(int a)
{
    wins[a].open = 0;
    if (focused == a) focused = -1;
    dirty = 1;
}

static int window_at(int x, int y)
{
    int hit = -1;
    for (int i = 0; i < APP_COUNT; i++) {
        win_t *w = &wins[i];
        if (!w->open || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
            if (hit < 0 || w->z > wins[hit].z) hit = i;
    }
    return hit;
}

/* ------------------------------------------------------------------ drawing helpers */

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

static void button(int x, int y, int w, int h, const char *label, int on)
{
    gfx_rrect(&bs, x, y, w, h, 7, on ? C_ACCENT : C_BUTTON, 255);
    gfx_text_center(&bs, &ui_font_regular, x, y + (h - 17) / 2, w, label, C_TEXT, 255);
}

static void put_num(char *out, uint32_t v)
{
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int o = (int)strlen(out);
    while (n) out[o++] = t[--n];
    out[o] = 0;
}

static void put_ip(char *out, uint32_t ip)
{
    for (int i = 0; i < 4; i++) { put_num(out, (ip >> (8 * i)) & 255); if (i < 3) strcat(out, "."); }
}

static int inside(int x, int y, int bx, int by, int bw, int bh) { return x >= bx && x < bx + bw && y >= by && y < by + bh; }

/* ------------------------------------------------------------------ panels */

static void row(int x, int *y, const char *k, const char *v)
{
    gfx_text(&bs, &ui_font_regular, x, *y, k, C_DIM, 255);
    gfx_text(&bs, &ui_font_regular, x + 130, *y, v, C_TEXT, 255);
    *y += 24;
}

static void draw_system(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, C_PANEL);
    gfx_blit_scaled(&bs, icon("launcher", 1), x + 24, y + 22, 56, 56, 255);
    gfx_text(&bs, &ui_font_title, x + 96, y + 24, "ArmtOS", C_TEXT, 255);
    gfx_text(&bs, &ui_font_regular, x + 96, y + 54, "tOS on the Raspberry Pi  ·  " TOS_VERSION, C_DIM, 255);
    int ly = y + 104, lx = x + 28;
    char v[64];
    row(lx, &ly, "Board", bcm_model_name());
    uint32_t rev = 0, w2[2];
    v[0] = 0;
    if (mailbox_board_revision(&rev) == 0) { strcpy(v, "0x"); for (int s = 28; s >= 0; s -= 4) { char c[2] = { "0123456789abcdef"[(rev >> s) & 15], 0 }; strcat(v, c); } }
    row(lx, &ly, "Revision", v);
    v[0] = 0; w2[0] = 3;
    if (mailbox_tag(0x00030002, w2, 1, 2) == 0) { put_num(v, w2[1] / 1000000); strcat(v, " MHz"); }
    row(lx, &ly, "ARM clock", v);
    uint32_t arm = 0;
    v[0] = 0;
    if (mailbox_arm_memory(&arm) == 0) { put_num(v, arm >> 20); strcat(v, " MB"); }
    row(lx, &ly, "Memory", v);
    v[0] = 0; w2[0] = 0;
    if (mailbox_tag(0x00010006, w2, 0, 2) == 0) { put_num(v, w2[1] >> 20); strcat(v, " MB"); }
    row(lx, &ly, "VideoCore", v);
    v[0] = 0; w2[0] = 0;
    if (mailbox_tag(0x00030006, w2, 1, 2) == 0) { put_num(v, w2[1] / 1000); strcat(v, " °C"); }
    row(lx, &ly, "Temperature", v);
    v[0] = 0;
    put_num(v, (uint32_t)SW); strcat(v, " x "); put_num(v, (uint32_t)SH);
    row(lx, &ly, "Display", v);
    v[0] = 0;
    uint32_t up = now_ms() / 1000;
    put_num(v, up / 60); strcat(v, " min "); put_num(v, up % 60); strcat(v, " s");
    row(lx, &ly, "Uptime", v);
    row(lx, &ly, "Input", usbhid_has_mouse() ? (usbhid_has_keyboard() ? "USB mouse and keyboard" : "USB mouse") :
                           usbhid_has_keyboard() ? "USB keyboard" : "serial console");
    dirty = 1;                       /* the uptime and temperature move */
}

/* Settings: display and the raspi-config interface switches */
static const int res_w[4] = { 1024, 1280, 1600, 1920 };
static const int res_h[4] = { 768, 720, 900, 1080 };
static int set_resolution(int w, int h);
static void set_wallpaper(int c);

static const char *const iface_name[4] = { "I2C", "SPI", "Serial (UART)", "1-Wire" };

static int iface_on(int i)
{
    char v[8];
    switch (i) {
    case 0: return cfg_dtparam_on("i2c_arm") || cfg_dtparam_on("i2c");
    case 1: return cfg_dtparam_on("spi");
    case 2: return cfg_get("enable_uart", v, sizeof(v)) && v[0] == '1';
    default: return cfg_has_prefix("dtoverlay=w1-gpio");
    }
}

static void iface_toggle(int i)
{
    if (cfg_load() != 0 && emmc_init() != 0) { strcpy(status_line, "No SD card: config.txt cannot be changed"); return; }
    int on = !iface_on(i);
    switch (i) {
    case 0: cfg_set("dtparam=i2c_arm", on ? "on" : "off"); break;
    case 1: cfg_set("dtparam=spi", on ? "on" : "off"); break;
    case 2: cfg_set("enable_uart", on ? "1" : "0"); break;
    default:
        if (on) { cfg_uncomment_prefix("dtoverlay=w1-gpio"); if (!cfg_has_prefix("dtoverlay=w1-gpio")) cfg_append_line("dtoverlay=w1-gpio"); }
        else cfg_comment_prefix("dtoverlay=w1-gpio");
    }
    if (cfg_save() == 0) { reboot_needed = 1; strcpy(status_line, "Saved to config.txt -- restart to apply"); }
    else strcpy(status_line, "Could not write config.txt");
}

static void draw_settings(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, C_PANEL);
    int px = x + 24, py = y + 18;
    gfx_text(&bs, &ui_font_title, px, py, "Settings", C_TEXT, 255);
    py += 44;
    gfx_text(&bs, &ui_font_small, px, py, "DISPLAY", C_DIM, 255); py += 22;
    for (int i = 0; i < 4; i++) {
        char l[16] = "";
        put_num(l, (uint32_t)res_w[i]); strcat(l, "x"); put_num(l, (uint32_t)res_h[i]);
        button(px + i * 134, py, 126, 32, l, SW == res_w[i] && SH == res_h[i]);
    }
    py += 52;
    gfx_text(&bs, &ui_font_small, px, py, "WALLPAPER", C_DIM, 255); py += 22;
    static const char *wn[3] = { "Dusk", "Ocean", "Graphite" };
    static const uint32_t wa[3][2] = { { 0x302468, 0xE07A6E }, { 0x0B3A66, 0x2BB3C9 }, { 0x2A2C33, 0x5A5F6B } };
    for (int i = 0; i < 3; i++) {
        int bx = px + i * 150;
        gfx_rrect(&bs, bx - 3, py - 3, 136, 68, 10, wallpaper_choice == i ? C_ACCENT : C_PANEL, 255);
        gfx_surface_t t = bs;
        gfx_clip(&t, bx, py, 130, 62);
        gfx_gradient_v(&t, bx, py, 130, 62, wa[i][0], wa[i][1]);
        gfx_text_center(&bs, &ui_font_regular, bx, py + 70, 130, wn[i], C_TEXT, 255);
    }
    py += 104;
    gfx_text(&bs, &ui_font_small, px, py, "INTERFACES  (config.txt, as raspi-config sets them)", C_DIM, 255); py += 22;
    static int loaded;
    if (!loaded) { cfg_load(); loaded = 1; }
    for (int i = 0; i < 4; i++) {
        int bx = px + (i % 2) * 276, by = py + (i / 2) * 42;
        char l[40];
        strcpy(l, iface_name[i]);
        strcat(l, iface_on(i) ? ":  On" : ":  Off");
        button(bx, by, 264, 32, l, iface_on(i));
    }
    py += 92;
    if (status_line[0]) gfx_text(&bs, &ui_font_regular, px, py, status_line, reboot_needed ? 0xFDE047 : C_DIM, 255);
    (void)w; (void)h;
}

static void click_settings(int lx, int ly)
{
    int px = 24, py = 18 + 44 + 22;
    for (int i = 0; i < 4; i++) if (inside(lx, ly, px + i * 134, py, 126, 32)) { set_resolution(res_w[i], res_h[i]); return; }
    py += 52 + 22;
    for (int i = 0; i < 3; i++) if (inside(lx, ly, px + i * 150, py, 130, 62)) { set_wallpaper(i); return; }
    py += 104 + 22;
    for (int i = 0; i < 4; i++)
        if (inside(lx, ly, px + (i % 2) * 276, py + (i / 2) * 42, 264, 32)) { iface_toggle(i); return; }
}

/* GPIO: the 40-pin header as it sits on the board */
static const int8_t header[40] = {   /* -1 3V3, -2 5V, -3 GND, else BCM number */
    -1, -2, 2, -2, 3, -3, 4, 14, -3, 15, 17, 18, 27, -3, 22, 23, -1, 24, 10, -3,
    9, 25, 11, 8, -3, 7, 0, 1, 5, -3, 6, 12, 13, -3, 19, 16, 26, 20, -3, 21 };
static uint8_t pin_out[28];

#define PIN_R 11
static void pin_pos(int i, int x, int y, int *cx, int *cy)
{
    *cx = x + 240 + (i % 2) * 34;
    *cy = y + 34 + (i / 2) * 22;
}

static void draw_gpio(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, C_PANEL);
    gfx_rrect(&bs, x + 216, y + 16, 92, 22 * 20 + 16, 8, 0x0E3B1E, 255);       /* the board */
    if (!bcm_has_legacy_gpio) {
        gfx_text(&bs, &ui_font_regular, x + 24, y + 24, "The Pi 5's header is behind RP1;", C_TEXT, 255);
        gfx_text(&bs, &ui_font_regular, x + 24, y + 46, "this kernel does not drive it yet.", C_DIM, 255);
    }
    for (int i = 0; i < 40; i++) {
        int cx, cy;
        pin_pos(i, x, y, &cx, &cy);
        int p = header[i];
        uint32_t col = p == -1 ? 0xF59E0B : p == -2 ? 0xEF4444 : p == -3 ? 0x3F3F46 : 0x9CA3AF;
        int level = 0;
        if (p >= 0 && bcm_has_legacy_gpio) {
            level = hw_pin_read(p);
            col = level ? 0x22C55E : (pin_out[p] ? 0x166534 : 0x9CA3AF);
        }
        gfx_circle(&bs, cx, cy, 8, col, 255);
        char l[12] = "";
        if (p == -1) strcpy(l, "3V3");
        else if (p == -2) strcpy(l, "5V");
        else if (p == -3) strcpy(l, "GND");
        else { strcpy(l, "GPIO"); put_num(l, (uint32_t)p); }
        int tw = gfx_text_width(&ui_font_small, l);
        if (i % 2 == 0) gfx_text(&bs, &ui_font_small, cx - 18 - tw, cy - 8, l, p >= 0 ? C_TEXT : C_DIM, 255);
        else gfx_text(&bs, &ui_font_small, cx + 18, cy - 8, l, p >= 0 ? C_TEXT : C_DIM, 255);
    }
    gfx_text(&bs, &ui_font_small, x + 24, y + h - 50, "Click a GPIO pin to make it an output and flip it.", C_DIM, 255);
    gfx_text(&bs, &ui_font_small, x + 24, y + h - 30, "Green: high.  Grey: low.  Live, from the pin itself.", C_DIM, 255);
    dirty = 1;                       /* levels are read live */
}

static void click_gpio(int lx, int ly)
{
    if (!bcm_has_legacy_gpio) return;
    for (int i = 0; i < 40; i++) {
        int cx, cy;
        pin_pos(i, 0, 0, &cx, &cy);
        if ((lx - cx) * (lx - cx) + (ly - cy) * (ly - cy) > PIN_R * PIN_R) continue;
        int p = header[i];
        if (p < 0) return;
        if (!pin_out[p]) { hw_pin_mode(p, HW_PIN_OUT); pin_out[p] = 1; }
        hw_pin_write(p, !hw_pin_read(p));
        return;
    }
}

static void draw_network(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, C_PANEL);
    int px = x + 24, py = y + 18;
    gfx_text(&bs, &ui_font_title, px, py, "Network", C_TEXT, 255);
    py += 48;
    char v[64];
    int up = netcmd_is_up();
    row(px, &py, "Status", up ? (net_ip ? "Connected" : "Up, no address") : "Not started");
    row(px, &py, "Adapter", up ? nic_driver_name : "-");
    v[0] = 0;
    if (up) for (int k = 0; k < 6; k++) { char c[4] = { "0123456789abcdef"[net_mac[k] >> 4], "0123456789abcdef"[net_mac[k] & 15], k < 5 ? ':' : 0, 0 }; strcat(v, c); }
    row(px, &py, "MAC", up ? v : "-");
    v[0] = 0; if (up) put_ip(v, net_ip);
    row(px, &py, "Address", up ? v : "-");
    v[0] = 0; if (up) put_ip(v, net_gateway);
    row(px, &py, "Gateway", up ? v : "-");
    v[0] = 0; if (up) put_ip(v, net_dns);
    row(px, &py, "DNS", up ? v : "-");
    py += 12;
    button(px, py, 200, 34, up ? "Connected" : "Connect (DHCP)", up);
    button(px + 216, py, 200, 34, "Run nettest", 0);
    (void)w; (void)h;
}

static void click_network(int lx, int ly)
{
    int px = 24, py = 18 + 48 + 6 * 24 + 12;
    if (inside(lx, ly, px, py, 200, 34) && !netcmd_is_up()) {
        open_app(APP_TERMINAL);
        uart_puts("\n");
        netcmd_up();
    } else if (inside(lx, ly, px + 216, py, 200, 34)) {
        open_app(APP_TERMINAL);
        char a0[] = "nettest";
        char *argv[1] = { a0 };
        uart_puts("\nnettest\n");
        net_command(1, argv);
    }
    dirty = 1;
}

/* Files: what is in the root of the card */
static char fnames[24][16];
static uint32_t fsizes[24];
static int fcount = -1;

static void list_cb(const char *name, uint32_t size)
{
    if (fcount < 0 || fcount >= 24) return;
    strncpy(fnames[fcount], name, 15);
    fnames[fcount][15] = 0;
    fsizes[fcount] = size;
    fcount++;
}

static void refresh_files(void)
{
    fcount = 0;
    if (emmc_init() != 0 || fat_mount() != 0) { fcount = -2; return; }
    fat_list(list_cb);
}

static int ends_with(const char *s, const char *suf)
{
    int a = (int)strlen(s), b = (int)strlen(suf);
    if (a < b) return 0;
    for (int i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != suf[i]) return 0;
    }
    return 1;
}

static void draw_files(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, C_PANEL);
    if (fcount == -1) refresh_files();
    int px = x + 20, py = y + 14;
    gfx_text(&bs, &ui_font_medium, px, py, "SD card  ·  boot partition", C_TEXT, 255);
    py += 30;
    if (fcount == -2) { gfx_text(&bs, &ui_font_regular, px, py, "No readable card.", C_DIM, 255); return; }
    for (int i = 0; i < fcount && py < y + h - 30; i++) {
        int prog = ends_with(fnames[i], ".t");
        if (i % 2 == 0) gfx_fill(&bs, x + 10, py - 3, w - 20, 26, 0x24252C);
        gfx_blit(&bs, icon(prog ? "terminal" : "pdf", 0), px, py, 255);
        gfx_text(&bs, &ui_font_regular, px + 30, py + 1, fnames[i], C_TEXT, 255);
        char s[24] = "";
        put_num(s, fsizes[i]); strcat(s, " bytes");
        gfx_text(&bs, &ui_font_small, x + w - 30 - gfx_text_width(&ui_font_small, s), py + 3, s, C_DIM, 255);
        if (prog) gfx_text(&bs, &ui_font_small, x + w - 190, py + 3, "click to run", 0x60A5FA, 255);
        py += 26;
    }
}

static void click_files(int lx, int ly)
{
    int idx = (ly - 44 + 3) / 26;
    if (idx < 0 || idx >= fcount || !ends_with(fnames[idx], ".t")) { refresh_files(); dirty = 1; return; }
    static uint8_t image[256 * 1024] __attribute__((aligned(8)));
    int n = fat_read_file(fnames[idx], image, sizeof(image));
    if (n < 0) return;
    open_app(APP_TERMINAL);
    uart_puts("\nrun ");
    uart_puts(fnames[idx]);
    uart_puts("\n");
    loader_run(image, (uint32_t)n);
    uart_puts("\n");
}

/* ------------------------------------------------------------------ windows */

static const uint32_t pal[16] = {
    0x1A1B21, 0x3B82F6, 0x22C55E, 0x06B6D4, 0xEF4444, 0xD946EF, 0xF59E0B, 0xD4D4D8,
    0x71717A, 0x60A5FA, 0x4ADE80, 0x22D3EE, 0xF87171, 0xF0ABFC, 0xFDE047, 0xFAFAFA,
};

static void draw_terminal(int x, int y, int w, int h)
{
    gfx_fill(&bs, x, y, w, h, pal[0]);
    for (int r = 0; r < TROWS; r++) {
        int idx = r - tview;                     /* negative: from the scrollback */
        const uint8_t *line;
        if (idx >= 0) line = tcells[idx];
        else {
            int back_n = -idx;
            if (back_n > tscroll_count) { line = 0; }
            else line = tscroll[(tscroll_head - back_n + TSCROLL) % TSCROLL];
        }
        for (int c = 0; c < TCOLS; c++) {
            uint8_t ch = line ? line[c] : ' ';
            int cursor = tview == 0 && r == trow && c == tcol;
            gfx_cell(&bs, x + CPAD_X + c * CW, y + CPAD_Y + r * CH, ch,
                     cursor ? pal[0] : pal[7], cursor ? pal[7] : pal[0]);
        }
    }
}

static void traffic_lights(int x, int y, int on)
{
    static const uint32_t col[3] = { 0xFF5F57, 0xFEBC2E, 0x28C840 };
    for (int i = 0; i < 3; i++) gfx_circle(&bs, x + 18 + i * 20, y + TITLE_H / 2, 6, on ? col[i] : 0x4B4D57, 255);
}

static void draw_window(int a)
{
    win_t *w = &wins[a];
    int x = w->x, y = w->y, ww = w->w, hh = w->h, on = (a == focused);
    gfx_shadow(&bs, x, y, ww, hh, on ? 230 : 130);
    gfx_rrect(&bs, x, y, ww, hh, RADIUS, a == APP_TERMINAL ? pal[0] : C_PANEL, 255);
    gfx_rrect(&bs, x, y, ww, TITLE_H + RADIUS, RADIUS, on ? 0x2C2E37 : 0x24252C, 255);
    gfx_hline(&bs, x + RADIUS, y, ww - 2 * RADIUS, 0xFFFFFF, 26);
    traffic_lights(x, y, on);
    int tw = gfx_text_width(&ui_font_medium, apps[a].title);
    int tx = x + (ww - tw) / 2;
    gfx_blit(&bs, icon(apps[a].icon, 0), tx - 26, y + (TITLE_H - 20) / 2, 255);
    gfx_text(&bs, &ui_font_medium, tx, y + 8, apps[a].title, on ? C_TEXT : C_DIM, 255);
    gfx_clip(&bs, x, y + TITLE_H, ww, hh - TITLE_H - RADIUS / 2);
    int cx = x, cy = y + TITLE_H, cw = ww, ch = hh - TITLE_H;
    switch (a) {
    case APP_TERMINAL: draw_terminal(cx, cy, cw, ch); break;
    case APP_SYSTEM:   draw_system(cx, cy, cw, ch); break;
    case APP_SETTINGS: draw_settings(cx, cy, cw, ch); break;
    case APP_GPIO:     draw_gpio(cx, cy, cw, ch); break;
    case APP_NETWORK:  draw_network(cx, cy, cw, ch); break;
    case APP_FILES:    draw_files(cx, cy, cw, ch); break;
    }
    gfx_unclip(&bs);
    gfx_hline(&bs, x, y + TITLE_H, ww, 0, 90);
    gfx_rrect_outline(&bs, x, y, ww, hh, RADIUS, 0xFFFFFF, 30);
}

/* ------------------------------------------------------------------ bars, dock, launcher */

static const int dock_apps[] = { -1, APP_TERMINAL, APP_FILES, APP_GPIO, APP_NETWORK, APP_SYSTEM, APP_SETTINGS };
#define NDOCK ((int)(sizeof(dock_apps) / sizeof(dock_apps[0])))
static int dock_x[NDOCK], dock_sz[NDOCK], dock_left, dock_w, dock_top;

static void layout_dock(void)
{
    int total = DOCK_PAD * 2;
    for (int i = 0; i < NDOCK; i++) {
        int d = hover_dock < 0 ? 99 : (i > hover_dock ? i - hover_dock : hover_dock - i);
        dock_sz[i] = d == 0 ? 60 : d == 1 ? 54 : DOCK_ICON;
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
        int a = dock_apps[i];
        int sz = dock_sz[i];
        gfx_blit_scaled(&bs, icon(a < 0 ? "launcher" : apps[a].icon, 1), dock_x[i], dock_top + DOCK_H - DOCK_PAD - sz, sz, sz, 255);
        if (a >= 0 && wins[a].open) gfx_circle(&bs, dock_x[i] + sz / 2, dock_top + DOCK_H - 4, 2, 0xFFFFFF, 220);
    }
    if (hover_dock >= 0 && !menu_open) {
        int a = dock_apps[hover_dock];
        const char *label = a < 0 ? "Applications" : apps[a].title;
        int tw = gfx_text_width(&ui_font_regular, label);
        int lx = dock_x[hover_dock] + dock_sz[hover_dock] / 2 - (tw + 20) / 2, ly = dock_top - 40;
        gfx_shadow(&bs, lx, ly, tw + 20, 26, 120);
        gfx_rrect(&bs, lx, ly, tw + 20, 26, 8, 0x2A2B33, 240);
        gfx_text(&bs, &ui_font_regular, lx + 10, ly + 5, label, C_TEXT, 255);
    }
}

static const char *const menu_labels[] = { "About ArmtOS", "Settings...", "Applications", "Restart" };
#define NMENU 4
#define MITEM 26

static int menu_hit(int x, int y)
{
    if (!menu_open || x < 8 || x >= 8 + 200) return -1;
    int i = (y - (TOPBAR_H + 6)) / MITEM;
    return (y >= TOPBAR_H + 6 && i >= 0 && i < NMENU) ? i : -1;
}

static void draw_topbar(void)
{
    glass(0, 0, SW, TOPBAR_H, 0, 0x101015, 120);
    gfx_hline(&bs, 0, TOPBAR_H - 1, SW, 0, 70);
    if (menu_open) gfx_rrect(&bs, 8, 3, 30, TOPBAR_H - 6, 6, 0xFFFFFF, 40);
    gfx_blit(&bs, gfx_image("logo"), 14, (TOPBAR_H - 18) / 2, 255);
    const char *app = focused >= 0 ? apps[focused].title : "ArmtOS";
    gfx_text(&bs, &ui_font_medium, 48, 6, app, C_TEXT, 255);
    gfx_text(&bs, &ui_font_regular, 48 + gfx_text_width(&ui_font_medium, app) + 22, 6, bcm_model_name(), C_DIM, 255);

    cmos_time_t t;
    cmos_get_time(&t);
    char clk[8];
    clk[0] = (char)('0' + t.hour / 10); clk[1] = (char)('0' + t.hour % 10); clk[2] = ':';
    clk[3] = (char)('0' + t.minute / 10); clk[4] = (char)('0' + t.minute % 10); clk[5] = 0;
    int rx = SW - 14 - gfx_text_width(&ui_font_medium, clk);
    gfx_text(&bs, &ui_font_medium, rx, 6, clk, C_TEXT, 255);
    rx -= 30;
    gfx_blit(&bs, gfx_image("glyph_search"), rx, 6, 230);
    rx -= 28;
    gfx_blit(&bs, gfx_image("glyph_ethernet-port"), rx, 6, netcmd_is_up() && net_ip ? 230 : 90);
    rx -= 28;
    gfx_blit(&bs, gfx_image("glyph_cpu"), rx, 6, 200);
    if (reboot_needed) {
        const char *r = "Restart to apply config.txt";
        rx -= gfx_text_width(&ui_font_small, r) + 18;
        gfx_text(&bs, &ui_font_small, rx, 7, r, 0xFDE047, 255);
    }

    if (menu_open) {
        int h = NMENU * MITEM + 10;
        gfx_shadow(&bs, 8, TOPBAR_H + 2, 200, h, 200);
        gfx_rrect(&bs, 8, TOPBAR_H + 2, 200, h, 9, 0x26272E, 245);
        for (int i = 0; i < NMENU; i++) {
            int iy = TOPBAR_H + 6 + i * MITEM;
            if (i == hover_menu) gfx_rrect(&bs, 13, iy, 190, MITEM, 6, C_ACCENT, 255);
            gfx_text(&bs, &ui_font_regular, 24, iy + 5, menu_labels[i], C_TEXT, 255);
        }
    }
}

#define L_COLS 3
#define L_CW 150
#define L_CH 120

static void launcher_origin(int *ox, int *oy)
{
    *ox = (SW - L_COLS * L_CW) / 2;
    *oy = (SH - 2 * L_CH) / 2 + 10;
}

static int launcher_hit(int x, int y)
{
    int ox, oy;
    launcher_origin(&ox, &oy);
    if (x < ox || y < oy) return -1;
    int c = (x - ox) / L_CW, r = (y - oy) / L_CH;
    if (c >= L_COLS || r >= 2) return -1;
    return r * L_COLS + c < APP_COUNT ? r * L_COLS + c : -1;
}

static void draw_launcher(void)
{
    memcpy(back, wall_blur, (size_t)SW * SH * 4);
    gfx_fill_alpha(&bs, 0, 0, SW, SH, 0, 90);
    int ox, oy;
    launcher_origin(&ox, &oy);
    gfx_text_center(&bs, &ui_font_title, 0, oy - 70, SW, "Applications", C_TEXT, 255);
    for (int i = 0; i < APP_COUNT; i++) {
        int cx = ox + (i % L_COLS) * L_CW, cy = oy + (i / L_COLS) * L_CH;
        if (i == hover_launch) gfx_rrect(&bs, cx + 10, cy, L_CW - 20, L_CH - 10, 14, 0xFFFFFF, 40);
        gfx_blit_scaled(&bs, icon(apps[i].icon, 1), cx + (L_CW - 64) / 2, cy + 10, 64, 64, 255);
        gfx_text_center(&bs, &ui_font_regular, cx, cy + 80, L_CW, apps[i].title, C_TEXT, 255);
    }
}

/* ------------------------------------------------------------------ frames */

static void compose(void)
{
    if (launcher_open) { draw_launcher(); return; }
    memcpy(back, wall, (size_t)SW * SH * 4);
    int order[APP_COUNT], n = 0;
    for (int i = 0; i < APP_COUNT; i++) if (wins[i].open && !wins[i].minimized) order[n++] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i], j = i - 1;
        while (j >= 0 && wins[order[j]].z > wins[k].z) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    for (int i = 0; i < n; i++) draw_window(order[i]);
    draw_dock();
    draw_topbar();
}

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

static void present(void)
{
    copy_out(0, 0, SW, SH);
    cursor_draw();
}

static void redraw_soon(void)
{
    if (!active) return;
    dirty = 1;
    /* while a long command prints, the desktop is not ticking; repaint now and then from here */
    uint32_t t = now_ms();
    if (t - last_term_ms > 120) {
        last_term_ms = t;
        compose();
        present();
        dirty = 0;
    }
}

/* ------------------------------------------------------------------ wallpaper, mode */

static void set_wallpaper(int c)
{
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
    for (int i = 0; i < APP_COUNT; i++) {
        if (wins[i].x + wins[i].w > SW) wins[i].x = SW - wins[i].w > 0 ? SW - wins[i].w : 0;
        if (wins[i].y + wins[i].h > SH - DOCK_H) wins[i].y = TOPBAR_H + 6;
    }
    dirty = 1;
    return 0;
}

/* ------------------------------------------------------------------ input */

static void press(int x, int y)
{
    if (menu_open) {
        int i = menu_hit(x, y);
        menu_open = 0;
        dirty = 1;
        if (i == 0) open_app(APP_SYSTEM);
        else if (i == 1) open_app(APP_SETTINGS);
        else if (i == 2) launcher_open = 1;
        else if (i == 3) {
            uart_puts("restarting\n");
            mmio_write(bcm_periph_base + 0x10001c, 0x5A000020 | 1);
            mmio_write(bcm_periph_base + 0x100024, 0x5A000000 | (1 << 5));
            for (;;) __asm__ volatile("wfe");
        }
        if (i >= 0 || x < 40) return;
    }
    if (launcher_open) {
        int i = launcher_hit(x, y);
        launcher_open = 0;
        dirty = 1;
        if (i >= 0) open_app(i);
        return;
    }
    if (y < TOPBAR_H) {
        if (x < 40) { menu_open = 1; hover_menu = -1; dirty = 1; }
        else if (x > SW - 90 && x < SW - 60) { launcher_open = 1; dirty = 1; }
        return;
    }
    int d = dock_hit(x, y);
    if (d >= 0) {
        int a = dock_apps[d];
        if (a < 0) launcher_open = 1;
        else if (wins[a].open && focused == a && !wins[a].minimized) wins[a].minimized = 1;
        else open_app(a);
        dirty = 1;
        return;
    }
    int a = window_at(x, y);
    if (a < 0) return;
    focus(a);
    win_t *w = &wins[a];
    int lx = x - w->x, ly = y - w->y;
    if (ly < TITLE_H) {
        for (int i = 0; i < 3; i++) {
            int cx = 18 + i * 20, cy = TITLE_H / 2;
            if ((lx - cx) * (lx - cx) + (ly - cy) * (ly - cy) <= 64) {
                if (i == 0) close_app(a); else w->minimized = 1;
                return;
            }
        }
        drag = a; drag_dx = lx; drag_dy = ly;
        return;
    }
    ly -= TITLE_H;
    switch (a) {
    case APP_SETTINGS: click_settings(lx, ly); break;
    case APP_GPIO:     click_gpio(lx, ly); break;
    case APP_NETWORK:  click_network(lx, ly); break;
    case APP_FILES:    click_files(lx, ly); break;
    }
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
        put_num(b, (uint32_t)mx); strcat(b, " "); put_num(b, (uint32_t)my); strcat(b, "\n");
        void (*m)(char) = uart_mirror_get();
        uart_mirror_set(0);
        uart_puts(b);
        uart_mirror_set(m);
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
    } else if (pressed) press(mx, my);

    int wheel = usbhid_wheel();
    if (wheel && window_at(mx, my) == APP_TERMINAL) {
        tview += wheel * 2;
        if (tview < 0) tview = 0;
        if (tview > tscroll_count) tview = tscroll_count;
        dirty = 1;
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
    if (term_dirty) { term_dirty = 0; dirty = 1; }

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
        /* only the pointer moved: put back what was under it, draw it at the new place */
        copy_out(cur_x, cur_y, 26, 28);
        cursor_draw();
    }
}

/* keys typed into the desktop go to the shell when the Terminal has the focus */
int desktop_getc(void)
{
    if (!active) return -1;
    int c = usbhid_getc();
    if (c < 0) return -1;
    if (focused != APP_TERMINAL) {
        if (c == '\n' || c == ' ') { open_app(APP_TERMINAL); }
        return -1;
    }
    if (c == USBHID_KEY_UP) { tview++; if (tview > tscroll_count) tview = tscroll_count; dirty = 1; return -1; }
    if (c == USBHID_KEY_DOWN) { if (tview) tview--; dirty = 1; return -1; }
    if (c >= 0x11 && c <= 0x14) return -1;
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

    if (usb_ensure() == 0) usbhid_init();
    usbhid_set_screen(SW, SH);
    mx = SW / 2; my = SH / 2;

    memset(tcells, ' ', sizeof(tcells));
    trow = tcol = 0;
    uart_mirror = term_putc;
    active = 1;
    open_app(APP_TERMINAL);
    compose();
    present();
    uart_puts("desktop: ");
    put_num(status_line, 0); status_line[0] = 0;
    char m[64] = "";
    put_num(m, (uint32_t)SW); strcat(m, "x"); put_num(m, (uint32_t)SH);
    uart_puts(m);
    uart_puts(usbhid_has_mouse() ? ", USB mouse" : ", no mouse");
    uart_puts(usbhid_has_keyboard() ? ", USB keyboard\n" : ", no keyboard (type on the serial console)\n");
    return 0;
}
