#include "bcm.h"
#include "uart.h"
#include "mailbox.h"
#include "gpio.h"
#include "dwc2.h"
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

    uart_puts("\n[OK] early boot reached C\n");

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
