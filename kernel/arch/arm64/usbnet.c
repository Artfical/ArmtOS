#include "ethdev.h"
#include "dwc2.h"
#include "uart.h"
#include "irq.h"
#include "string_arm.h"

extern int net_verbose;

/*
 * USB Ethernet, class CDC-ECM: the generic "network adapter" most USB dongles, phones in
 * tethering mode and QEMU's usb-net present. The device exposes a pair of bulk endpoints and
 * carries each Ethernet frame as one bulk transfer, with no header at all.
 *
 * Which configuration, interface and alternate setting have the endpoints is read from the
 * descriptors rather than assumed, because dongles differ: many only expose the bulk pair on
 * alternate setting 1 of the data interface.
 */

static usb_device_t *dev;
static uint8_t our_mac[6];
static uint8_t ep_in, ep_out;
static int started;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct {
    int config_value, data_iface, data_alt, comm_iface, mac_string;
    uint8_t in, out;
} ecm_t;

/* Walks one configuration descriptor for a data interface with a bulk pair. */
static int parse_config(const uint8_t *b, int len, ecm_t *r)
{
    int iface = -1, alt = 0, cls = 0;
    int in = 0, out = 0, found = 0;
    memset(r, 0, sizeof(*r));
    r->comm_iface = -1;
    r->mac_string = 0;
    r->config_value = b[5];
    for (int i = 0; i + 1 < len && b[i] >= 2; i += b[i]) {
        const uint8_t *d = b + i;
        if (i + d[0] > len) break;
        if (d[1] == 4 && d[0] >= 9) {                          /* interface */
            if (found) break;
            iface = d[2]; alt = d[3]; cls = d[5]; in = out = 0;
            if (cls == 2 && d[6] == 6) r->comm_iface = iface;  /* the ECM control interface */
        } else if (d[1] == 0x24 && d[0] >= 13 && d[2] == 0x0F) {  /* CDC Ethernet functional descriptor */
            r->mac_string = d[3];
        } else if (d[1] == 5 && d[0] >= 7 && (d[3] & 3) == 2) {   /* bulk endpoint */
            if (d[2] & 0x80) in = d[2]; else out = d[2];
            if (in && out && (cls == 0x0A || cls == 2)) {
                r->data_iface = iface; r->data_alt = alt; r->in = (uint8_t)in; r->out = (uint8_t)out;
                found = 1;
            }
        }
    }
    /* A bulk pair alone does not make it Ethernet: RNDIS (also class 2, vendor protocol) has the
     * same shape and speaks a different protocol. Require the ECM marker. */
    return found && (r->comm_iface >= 0 || r->mac_string);
}

static int usbnet_start(void)
{
    dev = 0;
    ecm_t e;
    uint8_t cfg[512];
    for (int i = 0; i < usb_device_count() && !dev; i++) {
        usb_device_t *d = (usb_device_t *)usb_device_get(i);
        if (!d || d->dev_class == 9) continue;                    /* hubs carry no frames */
        uint8_t dd[18];
        if (usb_control(d, 0x80, 6, (1 << 8), 0, dd, 18) < 18) continue;
        int nconf = dd[17];
        for (int c = 0; c < nconf && c < 4; c++) {
            int n = usb_get_config(d, c, cfg, sizeof(cfg));
            if (n < 9) continue;
            if (net_verbose) {
                uart_puts("usbnet: configuration descriptor:");
                for (int k = 0; k < n; k++) { uart_putc(k % 16 ? ' ' : '\n'); uart_put_hex(cfg[k]); }
                uart_puts("\n");
            }
            if (parse_config(cfg, n, &e)) { dev = d; break; }
        }
    }
    if (!dev) return -1;

    if (usb_set_config(dev, e.config_value) != 0) return -1;
    if (e.data_alt != 0) usb_set_interface(dev, e.data_iface, e.data_alt);
    ep_in = e.in;
    ep_out = e.out;

    /* Accept our own address and broadcasts; a device that does not know the request just
     * stalls it, which is harmless. */
    if (e.comm_iface >= 0) usb_control(dev, 0x21, 0x43, 0x000C, (uint16_t)e.comm_iface, 0, 0);

    char s[40];
    int ok = 0;
    if (e.mac_string && usb_get_string(dev, e.mac_string, s, sizeof(s)) >= 12) {
        ok = 1;
        for (int i = 0; i < 6; i++) {
            int hi = hexval(s[i * 2]), lo = hexval(s[i * 2 + 1]);
            if (hi < 0 || lo < 0) { ok = 0; break; }
            our_mac[i] = (uint8_t)(hi << 4 | lo);
        }
    }
    if (!ok) {
        static const uint8_t fb[6] = { 0x02, 0x41, 0x52, 0x4D, 0x55, 0x53 };
        memcpy(our_mac, fb, 6);
        uart_puts("usbnet: the adapter gave no MAC address, using a made-up one\n");
    }
    started = 1;
    return 0;
}

static int usbnet_send(const uint8_t *frame, int len)
{
    if (!started || len <= 0 || len > ETH_MTU) return -1;
    static uint8_t out[ETH_MTU];
    memcpy(out, frame, (size_t)len);
    return usb_bulk(dev, ep_out, out, len) < 0 ? -1 : len;
}

static int usbnet_recv(uint8_t *frame, int max)
{
    if (!started) return 0;
    int n = usb_bulk_in_poll(dev, ep_in, frame, max);
    return n > 0 ? n : 0;
}

eth_driver_t usbnet_driver = { "USB Ethernet (CDC-ECM)", usbnet_start, usbnet_send, usbnet_recv, {0} };

void usbnet_publish_mac(uint8_t *out) { memcpy(out, our_mac, 6); }
