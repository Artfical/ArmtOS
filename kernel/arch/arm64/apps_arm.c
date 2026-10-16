#include <stdint.h>
#include "bcm.h"
#include "uart.h"
#include "irq.h"
#include "mailbox.h"
#include "hwapi.h"
#include "cfgfile.h"
#include "emmc.h"
#include "usbhid.h"
#include "version.h"
#include "string_arm.h"
#include "../../display/uiapp.h"
#include "../../display/uihost.h"

/*
 * The Pi's own desktop apps, on the same framework as the shared ones (uiapp.h): the Terminal
 * (which is the shell), System (what the firmware reports), GPIO (the 40-pin header, live) and
 * Raspberry Pi (raspi-config's interface switches, written to config.txt).
 */

/* ------------------------------------------------------------------ Terminal */

#define TCOLS 80
#define TROWS 24
#define TSCROLL 300
static uint8_t tcells[TROWS][TCOLS];
static uint8_t tscroll[TSCROLL][TCOLS];
static int tscroll_head, tscroll_count, tview, trow, tcol;
int term_changed;                    /* desktop.c repaints when this is set */

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

void term_reset(void) { memset(tcells, ' ', sizeof(tcells)); trow = tcol = 0; }

/* everything the shell prints comes here too (uart_mirror) */
void term_putc(char c)
{
    if (c == '\r') tcol = 0;
    else if (c == '\n') term_newline();
    else if (c == '\b') { if (tcol > 0) tcol--; }
    else if (c == '\t') { do { tcells[trow][tcol++] = ' '; } while (tcol % 8 && tcol < TCOLS); }
    else if ((unsigned char)c >= 32) {
        if (tcol >= TCOLS) term_newline();
        tcells[trow][tcol++] = (uint8_t)c;
    }
    tview = 0;
    term_changed = 1;
}

static void t_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    (void)a;
    gfx_fill(s, x, y, w, h, 0x1A1B21);
    for (int r = 0; r < TROWS; r++) {
        int idx = r - tview;
        const uint8_t *line = 0;
        if (idx >= 0) line = tcells[idx];
        else if (-idx <= tscroll_count) line = tscroll[(tscroll_head + idx + TSCROLL) % TSCROLL];
        for (int c = 0; c < TCOLS; c++) {
            int cur = tview == 0 && r == trow && c == tcol;
            gfx_cell(s, x + 10 + c * UI_CELL_W, y + 6 + r * UI_CELL_H, line ? line[c] : ' ',
                     cur ? 0x1A1B21 : 0xD4D4D8, cur ? 0xD4D4D8 : 0x1A1B21);
        }
    }
}

static void t_wheel(uiapp_t *a, int d)
{
    tview += d * 2;
    if (tview < 0) tview = 0;
    if (tview > tscroll_count) tview = tscroll_count;
    a->dirty = 1;
}

static int t_tick(uiapp_t *a) { (void)a; if (term_changed) { term_changed = 0; return 1; } return 0; }

const uiapp_class_t app_terminal_arm = {
    "terminal", "Terminal", "terminal", TCOLS * UI_CELL_W + 20, TROWS * UI_CELL_H + 12, 0, 4,
    0, t_draw, 0, 0, 0, 0, t_wheel, t_tick, 0,
};

/* ------------------------------------------------------------------ System */

static void row(gfx_surface_t *s, int x, int *y, int w, const char *k, const char *v)
{
    gfx_text(s, &ui_font_regular, x + 14, *y + 13, k, UI_TEXT, 255);
    gfx_text(s, &ui_font_regular, x + w - 14 - gfx_text_width(&ui_font_regular, v), *y + 13, v, UI_DIM, 255);
    *y += 44;
}

static void sys_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    (void)a; (void)h;
    gfx_blit_scaled(s, gfx_image("icon48_launcher"), x + w / 2 - 36, y + 20, 72, 72, 255);
    gfx_text_center(s, &ui_font_huge, x, y + 96, w, "ArmtOS", UI_TEXT, 255);
    gfx_text_center(s, &ui_font_regular, x, y + 156, w, "tOS on the Raspberry Pi  \xC2\xB7  " TOS_VERSION, UI_DIM, 255);
    int px = x + 30, pw = w - 60, gy = y + 190;
    gfx_rrect(s, px, gy, pw, 6 * 44, 10, UI_CARD, 255);
    for (int i = 1; i < 6; i++) gfx_hline(s, px + 14, gy + i * 44, pw - 28, UI_LINE, 255);
    char v[48];
    row(s, px, &gy, pw, "Board", bcm_model_name());
    uint32_t w2[2];
    v[0] = 0; w2[0] = 3;
    if (mailbox_tag(0x00030002, w2, 1, 2) == 0) { ui_num(v, w2[1] / 1000000); strcat(v, " MHz"); } else strcpy(v, "-");
    row(s, px, &gy, pw, "Processor clock", v);
    uint32_t arm = 0;
    v[0] = 0;
    if (mailbox_arm_memory(&arm) == 0) ui_size(v, arm); else strcpy(v, "-");
    row(s, px, &gy, pw, "Memory", v);
    v[0] = 0; w2[0] = 0;
    if (mailbox_tag(0x00030006, w2, 1, 2) == 0) { ui_num(v, w2[1] / 1000); strcat(v, " \xC2\xB0" "C"); } else strcpy(v, "-");
    row(s, px, &gy, pw, "Temperature", v);
    v[0] = 0; ui_num(v, uihost_get("width")); strcat(v, " x "); ui_num(v, uihost_get("height"));
    row(s, px, &gy, pw, "Display", v);
    row(s, px, &gy, pw, "Input", usbhid_has_mouse() ? (usbhid_has_keyboard() ? "USB mouse and keyboard" : "USB mouse")
                                                    : usbhid_has_keyboard() ? "USB keyboard" : "serial console");
}

static int sys_tick(uiapp_t *a)
{
    static uint32_t last;
    uint32_t sec = uihost_ms() / 2000;
    (void)a;
    if (sec != last) { last = sec; return 1; }
    return 0;
}

const uiapp_class_t app_system = {
    "system", "About This Pi", "about", 460, 480, 0, 4,
    0, sys_draw, 0, 0, 0, 0, 0, sys_tick, 0,
};

/* ------------------------------------------------------------------ GPIO */

static const int8_t header[40] = {   /* -1 3V3, -2 5V, -3 GND, else BCM number */
    -1, -2, 2, -2, 3, -3, 4, 14, -3, 15, 17, 18, 27, -3, 22, 23, -1, 24, 10, -3,
    9, 25, 11, 8, -3, 7, 0, 1, 5, -3, 6, 12, 13, -3, 19, 16, 26, 20, -3, 21 };
static uint8_t pin_out[28];

static void pin_pos(int i, int *cx, int *cy) { *cx = 250 + (i % 2) * 34; *cy = 34 + (i / 2) * 22; }

static void gpio_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    (void)a;
    gfx_rrect(s, x + 226, y + 16, 92, 22 * 20 + 16, 8, 0x0E3B1E, 255);
    if (!bcm_has_legacy_gpio) {
        gfx_text(s, &ui_font_regular, x + 24, y + 24, "The Pi 5's header is behind RP1,", UI_TEXT, 255);
        gfx_text(s, &ui_font_regular, x + 24, y + 46, "which this kernel does not drive yet.", UI_DIM, 255);
    }
    for (int i = 0; i < 40; i++) {
        int cx, cy;
        pin_pos(i, &cx, &cy);
        cx += x; cy += y;
        int p = header[i];
        uint32_t col = p == -1 ? 0xF59E0B : p == -2 ? 0xEF4444 : p == -3 ? 0x3F3F46 : 0x9CA3AF;
        if (p >= 0 && bcm_has_legacy_gpio) col = hw_pin_read(p) ? 0x22C55E : (pin_out[p] ? 0x166534 : 0x9CA3AF);
        gfx_circle(s, cx, cy, 8, col, 255);
        char l[12];
        if (p == -1) strcpy(l, "3V3"); else if (p == -2) strcpy(l, "5V"); else if (p == -3) strcpy(l, "GND");
        else { strcpy(l, "GPIO"); ui_num(l, p); }
        int tw = gfx_text_width(&ui_font_small, l);
        gfx_text(s, &ui_font_small, i % 2 == 0 ? cx - 18 - tw : cx + 18, cy - 8, l, p >= 0 ? UI_TEXT : UI_DIM, 255);
    }
    gfx_text(s, &ui_font_small, x + 24, y + h - 46, "Click a GPIO pin to make it an output and flip it.", UI_DIM, 255);
    gfx_text(s, &ui_font_small, x + 24, y + h - 26, "Green: high.  Grey: low.  Read live from the pins.", UI_DIM, 255);
}

static void gpio_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    if (!bcm_has_legacy_gpio) return;
    for (int i = 0; i < 40; i++) {
        int cx, cy;
        pin_pos(i, &cx, &cy);
        if ((px - cx) * (px - cx) + (py - cy) * (py - cy) > 121) continue;
        int p = header[i];
        if (p < 0) return;
        if (!pin_out[p]) { hw_pin_mode(p, HW_PIN_OUT); pin_out[p] = 1; }
        hw_pin_write(p, !hw_pin_read(p));
        a->dirty = 1;
        return;
    }
}

static int gpio_tick(uiapp_t *a)
{
    static uint32_t last;
    uint32_t t = uihost_ms() / 250;
    (void)a;
    if (t != last) { last = t; return 1; }               /* levels are read live */
    return 0;
}

const uiapp_class_t app_gpio = {
    "gpio", "GPIO", "gpio", 540, 520, 0, 4,
    0, gpio_draw, gpio_press, 0, 0, 0, 0, gpio_tick, 0,
};

/* ------------------------------------------------------------------ Raspberry Pi (raspi-config) */

static const char *const iface_name[4] = { "I2C", "SPI", "Serial port (UART)", "1-Wire" };
static char rp_msg[80];
static int rp_reboot;

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
    if (cfg_load() != 0 && emmc_init() != 0) { strcpy(rp_msg, "No SD card: config.txt cannot be changed"); return; }
    int on = !iface_on(i);
    switch (i) {
    case 0: cfg_set("dtparam=i2c_arm", on ? "on" : "off"); break;
    case 1: cfg_set("dtparam=spi", on ? "on" : "off"); break;
    case 2: cfg_set("enable_uart", on ? "1" : "0"); break;
    default:
        if (on) { cfg_uncomment_prefix("dtoverlay=w1-gpio"); if (!cfg_has_prefix("dtoverlay=w1-gpio")) cfg_append_line("dtoverlay=w1-gpio"); }
        else cfg_comment_prefix("dtoverlay=w1-gpio");
    }
    if (cfg_save() == 0) { rp_reboot = 1; strcpy(rp_msg, "Saved to config.txt -- restart to apply"); }
    else strcpy(rp_msg, "Could not write config.txt");
}

static void rp_init(uiapp_t *a, const char *arg) { (void)a; (void)arg; cfg_load(); }

static void rp_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    (void)a; (void)h;
    gfx_text(s, &ui_font_title, x + 28, y + 18, "Interfaces", UI_TEXT, 255);
    gfx_text(s, &ui_font_small, x + 28, y + 50, "What raspi-config switches in config.txt; the firmware reads it at start-up.", UI_DIM, 255);
    int px = x + 28, pw = w - 56, gy = y + 80;
    gfx_rrect(s, px, gy, pw, 4 * 44, 10, UI_CARD, 255);
    for (int i = 0; i < 4; i++) {
        if (i) gfx_hline(s, px + 14, gy + i * 44, pw - 28, UI_LINE, 255);
        gfx_text(s, &ui_font_regular, px + 14, gy + i * 44 + 13, iface_name[i], UI_TEXT, 255);
        int on = iface_on(i), sx = px + pw - 54, sy = gy + i * 44 + 10;
        gfx_rrect(s, sx, sy, 40, 24, 12, on ? 0x34C759 : (ui_theme.dark ? 0x4A4B54 : 0xD9D9DE), 255);
        gfx_circle(s, on ? sx + 28 : sx + 12, sy + 12, 10, 0xFFFFFF, 255);
    }
    if (rp_msg[0]) gfx_text(s, &ui_font_regular, px, gy + 4 * 44 + 18, rp_msg, rp_reboot ? 0xEAB308 : UI_DIM, 255);
}

static void rp_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    int x0 = 28, pw = a->w - 56, gy = 80;
    for (int i = 0; i < 4; i++)
        if (ui_hit(px, py, x0 + pw - 54, gy + i * 44 + 10, 40, 24)) { iface_toggle(i); a->dirty = 1; }
}

const uiapp_class_t app_raspi = {
    "raspi", "Raspberry Pi", "settings", 520, 330, 0, 4,
    rp_init, rp_draw, rp_press, 0, 0, 0, 0, 0, 0,
};
