#include "usbhid.h"
#include "dwc2.h"
#include "uart.h"
#include "string_arm.h"

/*
 * USB mice and keyboards, through the HID boot protocol every one of them supports: a
 * keyboard sends eight bytes (modifiers, reserved, up to six keys held), a mouse three or
 * more (buttons, dx, dy, wheel). Absolute pointers (QEMU's usb-tablet, touch screens in
 * mouse mode) send buttons and two 16-bit coordinates instead, scaled 0..32767.
 */

static usb_pipe_t mouse_pipe = { 2, 0, 0, 3, 0, 0, 0 };
static usb_pipe_t kbd_pipe   = { 3, 0, 0, 3, 0, 0, 0 };
static int mouse_absolute;
static int have_mouse, have_kbd;

/* pointer state, in the units set by usbhid_set_screen */
static int px, py, pw = 640, ph = 480;
static uint8_t pbuttons;
static int pwheel;

/* typed characters, waiting to be read */
static char keyq[64];
static int kq_head, kq_tail;
static uint8_t last_keys[6];

static void key_push(char c)
{
    int n = (kq_head + 1) % (int)sizeof(keyq);
    if (n == kq_tail) return;
    keyq[kq_head] = c;
    kq_head = n;
}

int usbhid_getc(void)
{
    if (kq_head == kq_tail) return -1;
    char c = keyq[kq_tail];
    kq_tail = (kq_tail + 1) % (int)sizeof(keyq);
    return (unsigned char)c;
}

/* HID usage ids to ASCII on a US layout */
static const char plain[0x39] =
    "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char shifted[0x39] =
    "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

static void keyboard_report(const uint8_t *r, int n)
{
    if (n < 8) return;
    int shift = (r[0] & 0x22) != 0, ctrl = (r[0] & 0x11) != 0;
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k < 4) continue;
        int was = 0;
        for (int j = 0; j < 6; j++) if (last_keys[j] == k) was = 1;
        if (was) continue;                                   /* still held, not a new press */
        if (k < 0x39) {
            char c = shift ? shifted[k] : plain[k];
            if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
            if (c) key_push(c);
        } else if (k == 0x4F) key_push(USBHID_KEY_RIGHT);
        else if (k == 0x50) key_push(USBHID_KEY_LEFT);
        else if (k == 0x51) key_push(USBHID_KEY_DOWN);
        else if (k == 0x52) key_push(USBHID_KEY_UP);
        else if (k == 0x4C) key_push(127);                   /* delete */
    }
    memcpy(last_keys, r + 2, 6);
}

static void mouse_report(const uint8_t *r, int n)
{
    if (n < 3) return;
    pbuttons = r[0] & 7;
    if (mouse_absolute && n >= 5) {
        int ax = r[1] | (r[2] << 8), ay = r[3] | (r[4] << 8);
        px = ax * pw / 32768;
        py = ay * ph / 32768;
        if (n >= 6) pwheel += (int8_t)r[5];
    } else {
        int dx = (int8_t)r[1], dy = (int8_t)r[2];
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        px += ax > 6 ? dx * 2 : dx;                          /* a little acceleration */
        py += ay > 6 ? dy * 2 : dy;
        if (n >= 4) pwheel += (int8_t)r[3];
    }
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= pw) px = pw - 1;
    if (py >= ph) py = ph - 1;
}

void usbhid_poll(void)
{
    uint8_t r[16];
    if (have_mouse) {
        for (int i = 0; i < 4; i++) {                        /* catch up on several reports */
            int n = usb_pipe_poll(&mouse_pipe, r, sizeof(r));
            if (n <= 0) break;
            mouse_report(r, n);
        }
    }
    if (have_kbd) {
        int n = usb_pipe_poll(&kbd_pipe, r, sizeof(r));
        if (n > 0) keyboard_report(r, n);
    }
}

void usbhid_set_screen(int w, int h)
{
    pw = w; ph = h;
    px = w / 2; py = h / 2;
}

void usbhid_pointer(int *x, int *y, int *buttons)
{
    if (x) *x = px;
    if (y) *y = py;
    if (buttons) *buttons = pbuttons;
}

int usbhid_wheel(void) { int w = pwheel; pwheel = 0; return w; }
int usbhid_has_mouse(void) { return have_mouse; }
int usbhid_has_keyboard(void) { return have_kbd; }

/* Finds HID interfaces in a device's configuration and starts them. */
static void probe(usb_device_t *d)
{
    uint8_t cfg[256];
    int n = usb_get_config(d, 0, cfg, sizeof(cfg));
    if (n < 9) return;
    int iface = -1, proto = 0, cls = 0, configured = 0;
    for (int i = 0; i + 1 < n && cfg[i] >= 2; i += cfg[i]) {
        const uint8_t *e = cfg + i;
        if (e[1] == 4 && e[0] >= 9) { iface = e[2]; cls = e[5]; proto = e[7]; }
        else if (e[1] == 5 && e[0] >= 7 && cls == 3 && (e[2] & 0x80) && (e[3] & 3) == 3) {
            usb_pipe_t *p = 0;
            if (proto == 1 && !have_kbd) { p = &kbd_pipe; have_kbd = 1; }
            else if (proto != 1 && !have_mouse) { p = &mouse_pipe; have_mouse = 1; mouse_absolute = (proto == 0); }
            if (!p) continue;
            if (!configured) { usb_set_config(d, cfg[5]); configured = 1; }
            if (proto != 0) usb_control(d, 0x21, 0x0B, 0, (uint16_t)iface, 0, 0);    /* SET_PROTOCOL: boot */
            usb_control(d, 0x21, 0x0A, 0, (uint16_t)iface, 0, 0);                    /* SET_IDLE: only on change */
            p->dev = d;
            p->ep = e[2] & 0xF;
            p->maxp = e[4] | ((e[5] & 7) << 8);
            p->armed = 0;
        }
    }
}

int usbhid_init(void)
{
    have_mouse = have_kbd = 0;
    for (int i = 0; i < usb_device_count(); i++) {
        usb_device_t *d = (usb_device_t *)usb_device_get(i);
        if (d && d->dev_class != 9) probe(d);
    }
    return have_mouse || have_kbd ? 0 : -1;
}
