#include "mouse.h"
#include "usb_mouse.h"
#include "isr.h"
#include "irq.h"
#include "io.h"
#include "terminal.h"
#include "synaptics.h"
#include "alps.h"
#include "elantech.h"
#include "trackpoint.h"

#define MOUSE_IRQ 12
#define MOUSE_PORT 0x60
#define STATUS_PORT 0x64

static volatile int mouse_x = 40;
static volatile int mouse_y = 12;
static volatile uint8_t mouse_buttons = 0;
static volatile int mouse_click_pending = 0;
static volatile int mouse_rclick_pending = 0;
static int rclick_x = 0;
static int rclick_y = 0;
static volatile int click_x = 0;
static volatile int click_y = 0;
int mouse_initialized = 0;

static void mouse_wait(int type)
{
    int t = 100000;
    if (type == 0) {
        while (t-- && (inb(STATUS_PORT) & 2));
    } else {
        while (t-- && !(inb(STATUS_PORT) & 1));
    }
}

static void mouse_write(uint8_t val)
{
    mouse_wait(0);
    outb(STATUS_PORT, 0xD4);
    mouse_wait(0);
    outb(MOUSE_PORT, val);
}

static uint8_t mouse_read(void)
{
    mouse_wait(1);
    return inb(MOUSE_PORT);
}

static int mouse_packet_phase = 0;
static uint8_t mouse_packet[4];
static int mouse_has_wheel = 0;
static volatile int mouse_wheel_accum = 0;

#define MOUSE_SENS_DIV 4
static int accum_dx = 0;
static int accum_dy = 0;

/* The text desktop moves the pointer in character cells (80 x 25); the graphical one in
 * pixels, with a little acceleration so a 1280-pixel screen does not need a long desk. */
static int pixel_mode;
static int max_x = 80, max_y = 25;

void mouse_set_pixel_mode(int width, int height)
{
    pixel_mode = 1;
    max_x = width;
    max_y = height;
    mouse_x = width / 2;
    mouse_y = height / 2;
    accum_dx = accum_dy = 0;
}

static void mouse_apply_delta(int dx, int dy)
{
    int cell_dx, cell_dy;
    if (pixel_mode) {
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        cell_dx = ax > 6 ? dx * 2 : dx;
        cell_dy = ay > 6 ? dy * 2 : dy;
    } else {
        accum_dx += dx;
        accum_dy += dy;
        cell_dx = accum_dx / MOUSE_SENS_DIV;
        cell_dy = accum_dy / MOUSE_SENS_DIV;
        accum_dx -= cell_dx * MOUSE_SENS_DIV;
        accum_dy -= cell_dy * MOUSE_SENS_DIV;
    }

    int nx = mouse_x + cell_dx;
    if (nx < 0) nx = 0;
    if (nx >= max_x) nx = max_x - 1;
    mouse_x = nx;

    int ny = mouse_y + cell_dy;
    if (ny < 0) ny = 0;
    if (ny >= max_y) ny = max_y - 1;
    mouse_y = ny;
}

static void mouse_process_packet(void)
{
    int dx = (int)(int8_t)mouse_packet[1];
    int dy = -(int)(int8_t)mouse_packet[2];
    mouse_apply_delta(dx, dy);

    mouse_buttons = mouse_packet[0] & 0x07;

    if ((mouse_packet[0] & 1) && !mouse_click_pending) {
        mouse_click_pending = 1;
        click_x = mouse_x;
        click_y = mouse_y;
    }

    if ((mouse_packet[0] & 2) && !mouse_rclick_pending) {
        mouse_rclick_pending = 1;
        rclick_x = mouse_x;
        rclick_y = mouse_y;
    }

    if (mouse_has_wheel) {
        /* IntelliMouse wheel byte: low nibble is a signed -8..7 motion
         * count (the upper nibble is used by some 5-button mice for
         * extra buttons, which we don't support, so it's masked off). */
        int8_t raw = (int8_t)(mouse_packet[3] & 0x0F);
        if (raw & 0x08) raw |= 0xF0;
        if (raw != 0) mouse_wheel_accum += raw;
    }
}

static void mouse_callback(registers_t *regs)
{
    (void)regs;
    uint8_t status = inb(STATUS_PORT);
    if (!(status & 1)) return;
    if (!(status & 0x20)) return;

    uint8_t data = inb(MOUSE_PORT);

    switch (mouse_packet_phase) {
        case 0:
            if (!(data & 0x08)) return;
            mouse_packet[0] = data;
            mouse_packet_phase = 1;
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_packet_phase = 2;
            break;
        case 2:
            mouse_packet[2] = data;
            if (mouse_has_wheel) {
                mouse_packet_phase = 3;
                break;
            }
            mouse_packet_phase = 0;
            mouse_process_packet();
            break;
        case 3:
            mouse_packet[3] = data;
            mouse_packet_phase = 0;
            mouse_process_packet();
            break;
    }
}

static void mouse_enable_irq12(void)
{
    mouse_wait(0);
    outb(STATUS_PORT, 0x20);
    mouse_wait(1);
    uint8_t config = inb(MOUSE_PORT);

    config |= 0x02;

    mouse_wait(0);
    outb(STATUS_PORT, 0x60);
    mouse_wait(0);
    outb(MOUSE_PORT, config);
}

/* Probes for known PS/2 touchpad/pointing-stick vendor extensions. Whatever
   is found (or not), we always fall through to standard PS/2 relative
   reporting below -- vendor absolute/extended packet formats are never
   enabled, since they can't be validated without real hardware. */
static void detect_touchpad(void)
{
    if (synaptics_detect()) {
        terminal_writestring("mouse: Synaptics TouchPad detected\n");
    } else if (alps_detect()) {
        terminal_writestring("mouse: ALPS TouchPad detected\n");
    } else if (elantech_detect()) {
        terminal_writestring("mouse: Elantech TouchPad detected\n");
    } else if (trackpoint_detect()) {
        terminal_writestring("mouse: TrackPoint detected\n");
    } else {
        terminal_writestring("mouse: standard PS/2 mouse\n");
    }
}

/* Standard "Microsoft IntelliMouse" wheel-enable handshake: setting the
 * sample rate to 200, then 100, then 80 in succession (without any other
 * command in between) is the documented magic sequence real PS/2 mice
 * and every common emulator (QEMU, VirtualBox, Bochs) recognize as a
 * request to start sending a 4th wheel-motion byte per packet. Querying
 * the device ID (0xF2) afterwards confirms it: 0x03 means the mouse
 * switched into wheel mode, 0x00 means it ignored the sequence (a plain
 * 3-byte mouse) and we keep using the original packet format. */
static int mouse_detect_wheel(void)
{
    mouse_write(0xF3); mouse_read();
    mouse_write(200);  mouse_read();
    mouse_write(0xF3); mouse_read();
    mouse_write(100);  mouse_read();
    mouse_write(0xF3); mouse_read();
    mouse_write(80);   mouse_read();

    mouse_write(0xF2);
    mouse_read();
    uint8_t id = mouse_read();
    return id == 3;
}

void mouse_init(void)
{
    asm volatile("cli");
    mouse_wait(0);
    outb(STATUS_PORT, 0xA8);
    mouse_wait(0);

    mouse_write(0xF6);
    mouse_read();

    mouse_enable_irq12();

    detect_touchpad();

    mouse_has_wheel = mouse_detect_wheel();
    if (mouse_has_wheel) terminal_writestring("mouse: scroll wheel detected\n");

    mouse_write(0xF4);
    mouse_read();
    asm volatile("sti");

    isr_register_handler(32 + MOUSE_IRQ, mouse_callback);
    irq_ack(MOUSE_IRQ);

    mouse_initialized = 1;
    usb_mouse_init();
}

void mouse_poll(void)
{
    int dx, dy;
    uint8_t btns;
    if (usb_mouse_read(&dx, &dy, &btns)) {
        mouse_apply_delta(dx, -dy);

        mouse_buttons = btns & 0x07;

        if ((btns & 1) && !mouse_click_pending) {
            mouse_click_pending = 1;
            click_x = mouse_x;
            click_y = mouse_y;
        }

        if ((btns & 2) && !mouse_rclick_pending) {
            mouse_rclick_pending = 1;
            rclick_x = mouse_x;
            rclick_y = mouse_y;
        }
    }
}

void mouse_get_state(int *x, int *y, uint8_t *buttons)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
}

int mouse_clicked(void)
{
    return mouse_click_pending;
}

int mouse_get_click(int *x, int *y)
{
    if (!mouse_click_pending) return 0;
    if (x) *x = click_x;
    if (y) *y = click_y;
    mouse_click_pending = 0;
    return 1;
}

int mouse_get_rclick(int *x, int *y)
{
    if (!mouse_rclick_pending) return 0;
    if (x) *x = rclick_x;
    if (y) *y = rclick_y;
    mouse_rclick_pending = 0;
    return 1;
}

int mouse_get_wheel_delta(void)
{
    int d = mouse_wheel_accum;
    mouse_wheel_accum = 0;
    return d;
}
