#include "bcm.h"
#include "uart.h"
#include "mailbox.h"
#include "gpio.h"
#include "dwc2.h"
#include "irq.h"
#include "emmc.h"
#include "mmu.h"
#include "fb.h"
#include "pwm.h"
#include "i2c.h"
#include "spi.h"
#include "fat.h"
#include "loader.h"
#include "version.h"

/* What the firmware hands over, printed so a serial console shows the port is alive and
 * the board was identified before anything harder is attempted. */
static void banner(void)
{
    uart_puts("\n");
    uart_puts("  ArmtOS " TOS_VERSION " -- tOS on ARM\n");
    uart_puts("  board: ");
    uart_puts(bcm_model_name());
    uart_puts("\n  peripherals at ");
    uart_put_hex(bcm_periph_base);
    uart_puts("\n");
}

void shell(void);

void arm_main(void)
{
    bcm_detect();
    uart_init();
    banner();

    uint32_t rev = 0, sz = 0;
    if (mailbox_board_revision(&rev) == 0) {
        uart_puts("  board revision: ");
        uart_put_hex(rev);
        uart_puts("\n");
    }
    if (mailbox_arm_memory(&sz) == 0) {
        uart_puts("  memory: ");
        uart_put_dec(sz / (1024 * 1024));
        uart_puts(" MB\n");
    }

    mmu_init();
    uart_puts("[OK] MMU on, caches enabled\n");
    irq_init();
    timer_init(100);                      /* a 100 Hz tick, as tOS uses on x86 */
    uart_puts("\n[OK] early boot reached C\n");
    uart_puts("[OK] interrupts and timer running\n");

    /* The activity LED proves the GPIO block responds; it is on a different pin per model
     * (and on the Pi 3 it hangs off the GPIO expander, where writing here does nothing --
     * harmless either way). */
    gpio_set_function(29, GPIO_OUT);
    gpio_write(29, 1);

    uart_puts("\nCommands: help, gpio <pin> <0|1>, read <pin>, mem, reboot\n");
    shell();
}

/* ---- a small serial shell, so the board can be poked at before anything larger exists ---- */

static int line_read(char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = uart_getc_nonblock();
        if (c < 0) { __asm__ volatile("wfe"); continue; }
        if (c == '\r' || c == '\n') { uart_puts("\n"); buf[n] = 0; return n; }
        if ((c == 8 || c == 127) && n > 0) { n--; uart_puts("\b \b"); continue; }
        if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; uart_putc((char)c); }
    }
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* reads a non-negative decimal number, or -1 */
static int parse_int(const char *s)
{
    if (!*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 100000) return -1;
    }
    return v;
}

/* splits on single spaces, in place */
static int split(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

/* printed for each file the card holds */
static void list_entry(const char *name, uint32_t size)
{
    uart_puts("  ");
    uart_puts(name);
    uart_puts("  ");
    uart_put_dec(size);
    uart_puts(" bytes\n");
}

void shell(void)
{
    char line[128];
    char *argv[8];
    for (;;) {
        uart_puts("ArmtOS> ");
        line_read(line, (int)sizeof(line));
        int argc = split(line, argv, 8);
        if (argc == 0) continue;

        if (str_eq(argv[0], "help")) {
            uart_puts("help            this list\n");
            uart_puts("gpio PIN 0|1    drive a header pin low or high\n");
            uart_puts("read PIN        read a header pin\n");
            uart_puts("mem             how much memory the firmware gave the ARM side\n");
            uart_puts("board           which Raspberry Pi this is\n");
            uart_puts("usb             bring the USB controller up and list what is attached\n");
            uart_puts("uptime          how long the board has been running\n");
            uart_puts("sd              initialise the SD card and show its partitions\n");
            uart_puts("fb              put something on the screen\n");
            uart_puts("pwm PIN PCT     drive a pin as an analog level (12, 13, 18 or 19)\n");
            uart_puts("i2cscan         list the addresses answering on the I2C bus\n");
            uart_puts("adc CH          read a channel of an MCP3008 on SPI\n");
            uart_puts("ls              list the programs on the card\n");
            uart_puts("run NAME.t      run a program from the card\n");
            uart_puts("reboot          restart the board\n");
        } else if (str_eq(argv[0], "gpio") && argc >= 3) {
            int pin = parse_int(argv[1]), val = parse_int(argv[2]);
            if (pin < 0 || pin > 53 || (val != 0 && val != 1)) { uart_puts("usage: gpio PIN 0|1\n"); continue; }
            gpio_set_function(pin, GPIO_OUT);
            gpio_write(pin, val);
            uart_puts("GPIO ");
            uart_put_dec((uint64_t)pin);
            uart_puts(val ? " high\n" : " low\n");
        } else if (str_eq(argv[0], "read") && argc >= 2) {
            int pin = parse_int(argv[1]);
            if (pin < 0 || pin > 53) { uart_puts("usage: read PIN\n"); continue; }
            gpio_set_function(pin, GPIO_IN);
            uart_puts("GPIO ");
            uart_put_dec((uint64_t)pin);
            uart_puts(" = ");
            uart_put_dec((uint64_t)gpio_read(pin));
            uart_puts("\n");
        } else if (str_eq(argv[0], "mem")) {
            uint32_t sz = 0;
            if (mailbox_arm_memory(&sz) == 0) { uart_put_dec(sz / (1024 * 1024)); uart_puts(" MB\n"); }
            else uart_puts("the firmware did not answer\n");
        } else if (str_eq(argv[0], "board")) {
            uart_puts(bcm_model_name());
            uart_puts("\n");
        } else if (str_eq(argv[0], "usb")) {
            if (dwc2_init() != 0) { uart_puts("usb: controller did not start\n"); continue; }
            int n = usb_enumerate();
            uart_put_dec((uint64_t)n);
            uart_puts(" device(s)\n");
            for (int i = 0; i < n; i++) {
                const usb_device_t *d = usb_device_get(i);
                uart_puts("  addr ");
                uart_put_dec(d->address);
                uart_puts(" vendor ");
                uart_put_hex(d->vendor);
                uart_puts(" product ");
                uart_put_hex(d->product);
                uart_puts(" class ");
                uart_put_hex(d->dev_class);
                uart_puts("\n");
            }
        } else if (str_eq(argv[0], "uptime")) {
            uint64_t us = timer_us();
            uart_put_dec(us / 1000000);
            uart_puts(".");
            uart_put_dec((us / 1000) % 1000);
            uart_puts(" s, ");
            uart_put_dec(timer_ticks());
            uart_puts(" ticks\n");
        } else if (str_eq(argv[0], "sd")) {
            if (emmc_init() != 0) { uart_puts("sd: no card\n"); continue; }
            uart_puts("card: ");
            uart_put_dec(emmc_blocks() / 2048);
            uart_puts(" MB\n");

            static uint8_t mbr[512] __attribute__((aligned(8)));
            if (emmc_read(0, 1, mbr) < 0) { uart_puts("sd: read failed\n"); continue; }
            if (mbr[510] != 0x55 || mbr[511] != 0xAA) { uart_puts("sd: no partition table\n"); continue; }
            for (int pi = 0; pi < 4; pi++) {
                const uint8_t *e = mbr + 446 + pi * 16;
                uint32_t start = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                uint32_t sectors = (uint32_t)e[12] | ((uint32_t)e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
                if (e[4] == 0 || sectors == 0) continue;
                uart_puts("  partition ");
                uart_put_dec((uint64_t)pi + 1);
                uart_puts(": type ");
                uart_put_hex(e[4]);
                uart_puts(" start ");
                uart_put_dec(start);
                uart_puts(" size ");
                uart_put_dec(sectors / 2048);
                uart_puts(" MB\n");
            }
        } else if (str_eq(argv[0], "fb")) {
            if (!fb_ready() && fb_init(640, 480) != 0) { uart_puts("fb: no display\n"); continue; }
            fb_clear(0x00102040);
            fb_rect(40, 40, fb_width() - 80, 60, 0x00204080);
            fb_text(56, 64, "ArmtOS on the Raspberry Pi", 0x00FFFFFF);
            fb_text(56, 120, bcm_model_name(), 0x0080FF80);
            for (uint32_t i = 0; i < fb_width() - 80; i++)
                fb_pixel(40 + i, 160, 0x00FF8040);
            uart_puts("fb: ");
            uart_put_dec(fb_width());
            uart_puts("x");
            uart_put_dec(fb_height());
            uart_puts(" drawn\n");
        } else if (str_eq(argv[0], "pwm") && argc >= 3) {
            int pin = parse_int(argv[1]), pct = parse_int(argv[2]);
            if (pct < 0 || pct > 100) { uart_puts("usage: pwm PIN 0..100\n"); continue; }
            int ch = (pin == 12 || pin == 18) ? 0 : (pin == 13 || pin == 19) ? 1 : -1;
            if (ch < 0) { uart_puts("pwm: only GPIO 12, 13, 18 and 19 can do this\n"); continue; }
            if (pwm_init(ch, pin, 1024) != 0) { uart_puts("pwm: could not start\n"); continue; }
            pwm_percent(ch, (uint32_t)pct);
            uart_puts("GPIO ");
            uart_put_dec((uint64_t)pin);
            uart_puts(" at ");
            uart_put_dec((uint64_t)pct);
            uart_puts("%\n");
        } else if (str_eq(argv[0], "i2cscan")) {
            if (i2c_init(100000) != 0) { uart_puts("i2c: not available on this board\n"); continue; }
            uint8_t found[16];
            int n = i2c_scan(found, 16);
            if (n == 0) uart_puts("i2c: nothing answered\n");
            for (int k = 0; k < n; k++) { uart_puts("  "); uart_put_hex(found[k]); uart_puts("\n"); }
        } else if (str_eq(argv[0], "adc") && argc >= 2) {
            int ch = parse_int(argv[1]);
            if (ch < 0 || ch > 7) { uart_puts("usage: adc 0..7\n"); continue; }
            if (spi_init(1000000) != 0) { uart_puts("spi: not available on this board\n"); continue; }
            int v = spi_adc_read(0, ch);
            if (v < 0) { uart_puts("adc: no answer\n"); continue; }
            uart_puts("channel ");
            uart_put_dec((uint64_t)ch);
            uart_puts(" = ");
            uart_put_dec((uint64_t)v);
            uart_puts(" of 1023\n");
        } else if (str_eq(argv[0], "ls")) {
            if (emmc_init() != 0 || fat_mount() != 0) { uart_puts("ls: no readable card\n"); continue; }
            fat_list(list_entry);
        } else if (str_eq(argv[0], "run") && argc >= 2) {
            if (emmc_init() != 0 || fat_mount() != 0) { uart_puts("run: no readable card\n"); continue; }
            static uint8_t image[256 * 1024] __attribute__((aligned(8)));
            int n = fat_read_file(argv[1], image, sizeof(image));
            if (n < 0) { uart_puts("run: not found on the card\n"); continue; }
            loader_run(image, (uint32_t)n);
            uart_puts("\n");
        } else if (str_eq(argv[0], "reboot")) {
            uart_puts("rebooting\n");
            /* the watchdog, set to the shortest timeout, is how a Pi restarts itself */
            mmio_write(bcm_periph_base + 0x10001c, 0x5A000020 | 1);
            mmio_write(bcm_periph_base + 0x100024, 0x5A000000 | (1 << 5));
            for (;;) __asm__ volatile("wfe");
        } else {
            uart_puts("unknown command: ");
            uart_puts(argv[0]);
            uart_puts("\n");
        }
    }
}
