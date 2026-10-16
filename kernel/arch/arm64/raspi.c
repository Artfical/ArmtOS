#include <stdint.h>
#include "bcm.h"
#include "uart.h"
#include "irq.h"
#include "mailbox.h"
#include "cfgfile.h"
#include "string_arm.h"

int shell_readline(char *buf, int max);

/*
 * raspi-config for ArmtOS.
 *
 * The Raspberry Pi's own raspi-config (github.com/RPi-Distro/raspi-config) is a shell script
 * that, at its core, edits /boot/config.txt -- the file the firmware reads before the kernel
 * runs -- and then pokes Linux (modprobe, systemd, dpkg). The first half is the same on any
 * operating system and is ported here function for function, with the same names, the same
 * arguments and the same file edits, so `raspi-config nonint do_i2c 0` means what it means on
 * Raspberry Pi OS. The second half has no meaning here and is left out; what the kernel itself
 * can honour from config.txt (I2C/SPI switches, framebuffer size) it does (cfgfile.c).
 *
 *     raspi-config                     the menu
 *     raspi-config nonint FUNCTION...  the scriptable interface, e.g. do_spi 0 / get_spi
 *     raspi-config show                config.txt as it is on the card
 *     raspi-config info                what the firmware actually gave this board
 */

static int ask_reboot;

static void put_num(long v) { if (v < 0) { uart_putc('-'); v = -v; } uart_put_dec((uint64_t)v); }

static long to_num(const char *s, int *ok)
{
    long v = 0;
    int digits = 0;
    if (!s) { *ok = 0; return 0; }
    for (; *s >= '0' && *s <= '9'; s++) { v = v * 10 + (*s - '0'); digits++; }
    *ok = digits > 0 && !*s;
    return v;
}

static int load_for_edit(void)
{
    if (cfg_load() != 0 && !cfg_loaded()) {
        /* a card with no config.txt yet is fine -- the edit creates it; no card is not */
        extern int emmc_init(void);
        if (emmc_init() != 0) { uart_puts("raspi-config: no SD card to read config.txt from\n"); return -1; }
    }
    return 0;
}

static int commit(void)
{
    if (cfg_save() != 0) { uart_puts("raspi-config: could not write config.txt\n"); return 1; }
    return 0;
}

static int is_pifour(void) { return bcm_generation == 4; }

/* ---- the getters: print 0 when "on"/true, 1 when not -- raspi-config's convention ---- */

static int g_i2c(void)    { return cfg_dtparam_on("i2c_arm") || cfg_dtparam_on("i2c") ? 0 : 1; }
static int g_spi(void)    { return cfg_dtparam_on("spi") ? 0 : 1; }
static int g_serial_hw(void) { char v[8]; return cfg_get("enable_uart", v, sizeof(v)) && v[0] == '1' && !v[1] ? 0 : 1; }
static int g_onewire(void) { return cfg_has_prefix("dtoverlay=w1-gpio") ? 0 : 1; }
static int g_overscan(void) { char v[8]; return cfg_get("disable_overscan", v, sizeof(v)) && v[0] == '1' ? 1 : 0; }
static int g_composite(void) { return cfg_has_prefix("dtoverlay=vc4-kms-v3d,composite") ? 0 : 1; }
static int g_fan(void)    { return cfg_has_prefix("dtoverlay=gpio-fan") ? 0 : 1; }
static int g_camera(void) { char v[8]; return cfg_get("camera_auto_detect", v, sizeof(v)) && v[0] == '1' ? 0 :
                                                (cfg_get("start_x", v, sizeof(v)) && v[0] == '1' ? 0 : 1); }

static int g_leds(void)
{
    char v[32];
    if (!cfg_get("dtparam=act_led_trigger", v, sizeof(v))) return 1;     /* firmware default: constant */
    return !strcmp(v, "actpwr") ? 0 : !strcmp(v, "default-on") ? 1 : -1;
}

static int g_gpu_mem(void)
{
    char v[16];
    int ok;
    if (!cfg_get("gpu_mem", v, sizeof(v))) return 64;
    long n = to_num(v, &ok);
    return ok && n > 0 ? (int)n : 64;
}

/* gets the pi type the way raspi-config numbers it */
static int pi_type(void)
{
    switch (bcm_generation) { case 2: return 2; case 3: return 3; case 4: return 4; case 5: return 5; default: return -1; }
}

/* ---- the setters. `ret` is 0 for "yes, enable it" and 1 for "no", as in the shell ---- */

static void status(const char *what, int enabled)
{
    uart_puts(what);
    uart_puts(enabled ? " is enabled\n" : " is disabled\n");
}

static int d_i2c(int ret)
{
    if (ret > 1) return ret;
    cfg_set("dtparam=i2c_arm", ret == 0 ? "on" : "off");
    ask_reboot = 1;
    status("The ARM I2C interface", ret == 0);
    return commit();
}

static int d_spi(int ret)
{
    if (ret > 1) return ret;
    cfg_set("dtparam=spi", ret == 0 ? "on" : "off");
    ask_reboot = 1;
    status("The SPI interface", ret == 0);
    return commit();
}

static int d_serial_hw(int ret)
{
    if (ret > 1) return ret;
    cfg_set("enable_uart", ret == 0 ? "1" : "0");
    ask_reboot = 1;
    status("The serial port hardware (UART)", ret == 0);
    return commit();
}

static int d_onewire(int ret)
{
    if (ret > 1) return ret;
    if (ret == 0) {
        cfg_uncomment_prefix("dtoverlay=w1-gpio");
        if (!cfg_has_prefix("dtoverlay=w1-gpio")) cfg_append_line("dtoverlay=w1-gpio");
    } else cfg_comment_prefix("dtoverlay=w1-gpio");
    ask_reboot = 1;
    status("The one-wire interface", ret == 0);
    return commit();
}

static int d_overscan(int ret)
{
    if (ret > 1) return ret;
    if (ret == 0) cfg_set("disable_overscan", "0");
    else { cfg_comment_prefix("overscan_"); cfg_set("disable_overscan", "1"); }
    ask_reboot = 1;
    status("Display overscan compensation", ret == 0);
    return commit();
}

static int d_leds(int ret)
{
    if (ret > 1) return ret;
    const char *set = ret == 0 ? "actpwr" : "default-on";
    char line[64] = "dtparam=act_led_trigger=";
    strcpy(line + strlen(line), set);
    if (cfg_has_prefix("dtparam=act_led_trigger=")) cfg_replace_prefix("dtparam=act_led_trigger=", line);
    else cfg_append_line(line);
    uart_puts(ret == 0 ? "The power LED will flash for disk activity\n" : "The power LED will be on constantly\n");
    ask_reboot = 1;
    return commit();
}

static int d_memory_split(const char *arg)
{
    int ok;
    long mb = to_num(arg, &ok);
    if (!ok || mb < 16 || mb > 1024) { uart_puts("GPU memory must be a number from 16 to 1024 (MB)\n"); return 1; }
    cfg_clear("gpu_mem_1024");
    cfg_clear("gpu_mem_512");
    cfg_clear("gpu_mem_256");
    char v[12];
    int o = 0;
    char tmp[12];
    int t = 0;
    for (long n = mb; n > 0; n /= 10) tmp[t++] = (char)('0' + n % 10);
    while (t) v[o++] = tmp[--t];
    v[o] = 0;
    cfg_set("gpu_mem", v);
    ask_reboot = 1;
    uart_puts("GPU memory set to "); uart_puts(v); uart_puts(" MB\n");
    return commit();
}

static int d_camera(int ret)
{
    if (ret > 1) return ret;
    cfg_comment_prefix("startx");
    cfg_comment_prefix("fixup_file");
    if (ret == 0) {
        cfg_set("start_x", "1");
        if (g_gpu_mem() < 128) cfg_set("gpu_mem", "128");
        cfg_set("camera_auto_detect", "1");
    } else {
        cfg_set("start_x", "0");
        cfg_comment_prefix("start_file");
        cfg_set("camera_auto_detect", "0");
    }
    ask_reboot = 1;
    status("The camera interface", ret == 0);
    return commit();
}

static int d_composite(int ret)
{
    if (ret > 1) return ret;
    cfg_replace_prefix("dtoverlay=vc4-kms-v3d", ret == 0 ? "dtoverlay=vc4-kms-v3d,composite" : "dtoverlay=vc4-kms-v3d");
    ask_reboot = 1;
    status("Composite video", ret == 0);
    return commit();
}

static int d_pi4video(const char *v)
{
    if (!is_pifour()) { uart_puts("This option can only be used on a Pi 4\n"); return 1; }
    if (!strcmp(v, "V1")) {                                   /* HDMI 4Kp60 */
        cfg_set("hdmi_enable_4kp60", "1");
        cfg_comment_prefix("enable_tvout=");
        cfg_replace_prefix("dtoverlay=vc4-kms-v3d", "dtoverlay=vc4-kms-v3d");
    } else if (!strcmp(v, "V2")) {                            /* composite */
        cfg_set("enable_tvout", "1");
        cfg_comment_prefix("hdmi_enable_4kp60=");
        cfg_replace_prefix("dtoverlay=vc4-kms-v3d", "dtoverlay=vc4-kms-v3d,composite");
    } else if (!strcmp(v, "V3")) {                            /* default */
        cfg_comment_prefix("hdmi_enable_4kp60=");
        cfg_comment_prefix("enable_tvout=");
        cfg_replace_prefix("dtoverlay=vc4-kms-v3d", "dtoverlay=vc4-kms-v3d");
    } else { uart_puts("pi4video: V1 (HDMI 4Kp60), V2 (composite) or V3 (default)\n"); return 1; }
    ask_reboot = 1;
    return commit();
}

static int d_fan(int ret, const char *gpio_arg, const char *temp_arg)
{
    if (ret > 1) return ret;
    if (ret == 0) {
        int ok;
        long gpio = 14, tin = 80;
        if (gpio_arg) { gpio = to_num(gpio_arg, &ok); if (!ok || gpio < 2 || gpio > 27) { uart_puts("fan: GPIO must be 2-27\n"); return 1; } }
        if (temp_arg) { tin = to_num(temp_arg, &ok); if (!ok || tin < 60 || tin > 120) { uart_puts("fan: temperature must be 60-120 C\n"); return 1; } }
        char line[80] = "dtoverlay=gpio-fan,gpiopin=";
        int o = (int)strlen(line);
        char t[8]; int n;
        n = 0; for (long v = gpio; v > 0; v /= 10) t[n++] = (char)('0' + v % 10);
        while (n) line[o++] = t[--n];
        strcpy(line + o, ",temp="); o += 6;
        long tm = tin * 1000;
        n = 0; for (long v = tm; v > 0; v /= 10) t[n++] = (char)('0' + v % 10);
        while (n) line[o++] = t[--n];
        line[o] = 0;
        if (!cfg_has_prefix("dtoverlay=gpio-fan") && !strstr(cfg_text(), "dtoverlay=gpio-fan")) {
            const char *txt = cfg_text();
            int l = cfg_length();
            int last = l;
            while (last > 0 && (txt[last - 1] == '\n' || txt[last - 1] == '\r')) last--;
            int st = last;
            while (st > 0 && txt[st - 1] != '\n') st--;
            if (!(last - st >= 5 && !memcmp(txt + st, "[all]", 5))) cfg_append_line("[all]");
            cfg_append_line(line);
        } else cfg_replace_prefix("dtoverlay=gpio-fan", line);
    } else cfg_delete_containing("dtoverlay=gpio-fan");
    ask_reboot = 1;
    status("The cooling fan", ret == 0);
    return commit();
}

static int d_resolution(const char *g, const char *m)
{
    int ok1, ok2;
    long group = to_num(g, &ok1), mode = to_num(m, &ok2);
    if (!ok1 || !ok2) { uart_puts("resolution: GROUP MODE (group 1 = CEA, 2 = DMT; mode 0 = default)\n"); return 1; }
    if (mode == 0) {
        cfg_clear("hdmi_force_hotplug"); cfg_clear("hdmi_group"); cfg_clear("hdmi_mode");
    } else {
        cfg_set("hdmi_force_hotplug", "1");
        cfg_set("hdmi_group", g);
        cfg_set("hdmi_mode", m);
    }
    ask_reboot = 1;
    uart_puts(mode == 0 ? "HDMI mode: default\n" : group == 2 ? "HDMI mode: DMT " : "HDMI mode: CEA ");
    if (mode) { uart_put_dec((uint64_t)mode); uart_puts("\n"); }
    return commit();
}

/* ---- the scriptable interface ---- */

static int ret_arg(int argc, char **argv, int i) { int ok; long v = to_num(argc > i ? argv[i] : 0, &ok); return ok ? (int)v : 2; }

static int nonint(int argc, char **argv)
{
    const char *f = argv[0];
    if (load_for_edit() != 0) return 1;
    int r = 0;

    /* getters */
    if (!strcmp(f, "get_i2c"))         { put_num(g_i2c()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_spi"))         { put_num(g_spi()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_serial_hw") || !strcmp(f, "get_serial")) { put_num(f[10] == 'h' ? g_serial_hw() : 0); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_onewire"))     { put_num(g_onewire()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_overscan"))    { put_num(g_overscan()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_composite"))   { put_num(g_composite()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_fan"))         { put_num(g_fan()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_camera"))      { put_num(g_camera()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_leds"))        { put_num(g_leds()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_pi_type"))     { put_num(pi_type()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_current_memory_split") || !strcmp(f, "get_gpu_mem")) { put_num(g_gpu_mem()); uart_puts("\n"); return 0; }
    if (!strcmp(f, "get_fan_gpio")) {
        const char *t = strstr(cfg_text(), "dtoverlay=gpio-fan,gpiopin=");
        long v = 14;
        if (t) { int ok; char b[8]; int i = 0; for (t += 27; *t >= '0' && *t <= '9' && i < 7; t++) b[i++] = *t; b[i] = 0; v = to_num(b, &ok); if (!ok) v = 14; }
        put_num(v); uart_puts("\n"); return 0;
    }
    if (!strcmp(f, "get_fan_temp")) {
        const char *t = strstr(cfg_text(), ",temp=");
        long v = 80;
        if (t && strstr(cfg_text(), "dtoverlay=gpio-fan")) { int ok; char b[10]; int i = 0; for (t += 6; *t >= '0' && *t <= '9' && i < 9; t++) b[i++] = *t; b[i] = 0; v = to_num(b, &ok) / 1000; if (!ok) v = 80; }
        put_num(v); uart_puts("\n"); return 0;
    }
    if (!strcmp(f, "is_pifour"))  return is_pifour() ? 0 : 1;
    if (!strcmp(f, "is_pithree")) return bcm_generation == 3 ? 0 : 1;
    if (!strcmp(f, "is_pitwo"))   return bcm_generation == 2 ? 0 : 1;
    if (!strcmp(f, "is_pifive"))  return bcm_generation == 5 ? 0 : 1;
    if (!strcmp(f, "is_pi"))      return 0;

    /* raw config.txt access, as raspi-config exposes it internally */
    if (!strcmp(f, "get_config_var") && argc >= 2) {
        char v[96];
        if (cfg_get(argv[1], v, sizeof(v))) uart_puts(v); else uart_putc('0');
        uart_puts("\n");
        return 0;
    }
    if (!strcmp(f, "set_config_var") && argc >= 3) { cfg_set(argv[1], argv[2]); return commit(); }
    if (!strcmp(f, "clear_config_var") && argc >= 2) { cfg_clear(argv[1]); return commit(); }

    /* setters */
    if (!strcmp(f, "do_i2c"))          r = d_i2c(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_spi"))     r = d_spi(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_serial_hw") || !strcmp(f, "do_serial")) r = d_serial_hw(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_onewire")) r = d_onewire(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_overscan")) r = d_overscan(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_leds"))    r = d_leds(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_camera"))  r = d_camera(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_composite")) r = d_composite(ret_arg(argc, argv, 1));
    else if (!strcmp(f, "do_pi4video") && argc >= 2) r = d_pi4video(argv[1]);
    else if (!strcmp(f, "do_memory_split") && argc >= 2) r = d_memory_split(argv[1]);
    else if (!strcmp(f, "do_fan"))     r = d_fan(ret_arg(argc, argv, 1), argc > 2 ? argv[2] : 0, argc > 3 ? argv[3] : 0);
    else if (!strcmp(f, "do_resolution") && argc >= 3) r = d_resolution(argv[1], argv[2]);
    else {
        uart_puts("raspi-config: unknown or unsupported function '");
        uart_puts(f);
        uart_puts("' (Linux-only functions -- locale, wifi, users, ssh, overlay fs -- do not exist here)\n");
        return 1;
    }
    return r;
}

/* ---- information the firmware knows ---- */

static void info(void)
{
    uart_puts("Board       "); uart_puts(bcm_model_name()); uart_puts("\n");
    uint32_t rev;
    if (mailbox_board_revision(&rev) == 0) { uart_puts("Revision    "); uart_put_hex(rev); uart_puts("\n"); }
    uint32_t w[2];
    w[0] = 3;                                       /* clock id 3 = ARM */
    if (mailbox_tag(0x00030002, w, 1, 2) == 0) { uart_puts("ARM clock   "); uart_put_dec(w[1] / 1000000); uart_puts(" MHz\n"); }
    w[0] = 0;
    if (mailbox_tag(0x00010006, w, 0, 2) == 0)  { uart_puts("VideoCore   "); uart_put_dec(w[1] >> 20); uart_puts(" MB at "); uart_put_hex(w[0]); uart_puts("\n"); }
    uint32_t arm;
    if (mailbox_arm_memory(&arm) == 0) { uart_puts("ARM memory  "); uart_put_dec(arm >> 20); uart_puts(" MB\n"); }
    w[0] = 0;
    if (mailbox_tag(0x00030006, w, 1, 2) == 0) { uart_puts("Temperature "); uart_put_dec(w[1] / 1000); uart_puts(" C\n"); }
    w[0] = 0;
    if (mailbox_tag(0x00030046, w, 1, 1) == 0) { uart_puts("Throttled   "); uart_put_hex(w[0]); uart_puts(w[0] ? "  (under-voltage or throttling seen)\n" : "  (none)\n"); }
    uart_puts("config.txt  ");
    uart_puts(cfg_loaded() ? "read from the card\n" : "not found on the card\n");
}

/* ---- the menu ---- */

static int ask_yes(const char *q, int current_on)
{
    uart_puts(q);
    uart_puts(current_on ? " [Y/n] (now: on) " : " [y/N] (now: off) ");
    char l[16];
    shell_readline(l, sizeof(l));
    if (l[0] == 'y' || l[0] == 'Y') return 0;
    if (l[0] == 'n' || l[0] == 'N') return 1;
    return current_on ? 0 : 1;
}

static void interface_menu(void)
{
    for (;;) {
        uart_puts("\n  Interface Options\n"
                  "  1 SPI            ");  uart_puts(g_spi() ? "off\n" : "on\n");
        uart_puts("  2 I2C            ");  uart_puts(g_i2c() ? "off\n" : "on\n");
        uart_puts("  3 Serial port (UART)  "); uart_puts(g_serial_hw() ? "off\n" : "on\n");
        uart_puts("  4 1-Wire         ");  uart_puts(g_onewire() ? "off\n" : "on\n");
        uart_puts("  5 Camera         ");  uart_puts(g_camera() ? "off\n" : "on\n");
        uart_puts("  0 Back\n> ");
        char l[8];
        shell_readline(l, sizeof(l));
        switch (l[0]) {
        case '1': d_spi(ask_yes("Enable the SPI interface?", !g_spi())); break;
        case '2': d_i2c(ask_yes("Enable the ARM I2C interface?", !g_i2c())); break;
        case '3': d_serial_hw(ask_yes("Enable the serial port hardware?", !g_serial_hw())); break;
        case '4': d_onewire(ask_yes("Enable the one-wire interface?", !g_onewire())); break;
        case '5': d_camera(ask_yes("Enable the camera interface?", !g_camera())); break;
        case '0': case 0: return;
        }
    }
}

static void display_menu(void)
{
    for (;;) {
        uart_puts("\n  Display Options\n  1 Resolution (HDMI group/mode)\n  2 Overscan compensation  ");
        uart_puts(g_overscan() ? "off\n" : "on\n");
        if (is_pifour()) uart_puts("  3 Pi 4 video output (HDMI 4Kp60 / composite / default)\n");
        uart_puts("  0 Back\n> ");
        char l[8];
        shell_readline(l, sizeof(l));
        if (l[0] == '1') {
            char g[8], m[8];
            uart_puts("HDMI group (1 = CEA TV modes, 2 = DMT monitor modes): "); shell_readline(g, sizeof(g));
            uart_puts("HDMI mode (0 = default; e.g. CEA 16 = 1080p60, DMT 82 = 1080p60): "); shell_readline(m, sizeof(m));
            d_resolution(g, m);
        } else if (l[0] == '2') d_overscan(ask_yes("Compensate for overscan?", !g_overscan()));
        else if (l[0] == '3' && is_pifour()) {
            char v[8];
            uart_puts("V1 = HDMI 4Kp60, V2 = composite, V3 = default: "); shell_readline(v, sizeof(v));
            d_pi4video(v);
        } else if (l[0] == '0' || !l[0]) return;
    }
}

static void performance_menu(void)
{
    for (;;) {
        uart_puts("\n  Performance Options\n  1 GPU memory  (now ");
        put_num(g_gpu_mem());
        uart_puts(" MB)\n  2 Fan\n  3 Power LED\n  0 Back\n> ");
        char l[8];
        shell_readline(l, sizeof(l));
        if (l[0] == '1') {
            char v[12];
            uart_puts("GPU memory in MB: "); shell_readline(v, sizeof(v));
            d_memory_split(v);
        } else if (l[0] == '2') {
            int on = ask_yes("Enable the GPIO-controlled fan?", !g_fan());
            if (on == 0) {
                char gp[8], tp[8];
                uart_puts("GPIO pin (2-27, Enter = 14): "); shell_readline(gp, sizeof(gp));
                uart_puts("Switch on at (60-120 C, Enter = 80): "); shell_readline(tp, sizeof(tp));
                d_fan(0, gp[0] ? gp : 0, tp[0] ? tp : 0);
            } else d_fan(1, 0, 0);
        } else if (l[0] == '3') d_leds(ask_yes("Flash the power LED for disk activity?", g_leds() == 0));
        else if (l[0] == '0' || !l[0]) return;
    }
}

static void menu(void)
{
    if (load_for_edit() != 0) return;
    for (;;) {
        uart_puts("\n  Raspberry Pi Software Configuration Tool (raspi-config) -- ArmtOS\n"
                  "  1 Display Options\n"
                  "  2 Interface Options\n"
                  "  3 Performance Options\n"
                  "  4 Information (what the firmware reports)\n"
                  "  5 Show config.txt\n"
                  "  0 Finish\n> ");
        char l[8];
        shell_readline(l, sizeof(l));
        if (l[0] == '1') display_menu();
        else if (l[0] == '2') interface_menu();
        else if (l[0] == '3') performance_menu();
        else if (l[0] == '4') info();
        else if (l[0] == '5') uart_puts(cfg_text());
        else if (l[0] == '0' || !l[0]) break;
        if (cfg_load() != 0 && !cfg_loaded()) { }
    }
    if (ask_reboot) uart_puts("Changes take effect after a reboot -- type: reboot\n");
}

/* Returns 1 if the command was ours. */
int raspi_command(int argc, char **argv)
{
    if (strcmp(argv[0], "raspi-config") && strcmp(argv[0], "raspiconfig")) return 0;
    ask_reboot = 0;
    if (argc == 1) { menu(); return 1; }
    if (!strcmp(argv[1], "nonint") && argc >= 3) {
        int rc = nonint(argc - 2, argv + 2);
        if (ask_reboot) uart_puts("(a reboot is needed for this to take effect)\n");
        uart_puts(rc == 0 ? "" : "raspi-config: failed\n");
        return 1;
    }
    if (!strcmp(argv[1], "show")) { if (load_for_edit() == 0) uart_puts(cfg_text()); return 1; }
    if (!strcmp(argv[1], "info")) { cfg_load(); info(); return 1; }
    uart_puts("usage: raspi-config [nonint FUNCTION [ARGS] | show | info]\n"
              "  e.g. raspi-config nonint do_i2c 0     get_spi     do_memory_split 128\n"
              "       do_resolution 2 82   do_fan 0 14 70   set_config_var arm_boost 1\n");
    return 1;
}
