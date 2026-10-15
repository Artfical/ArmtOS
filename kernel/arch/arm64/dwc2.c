#include "dwc2.h"
#include "bcm.h"
#include "dma.h"
#include "string_arm.h"
#include "uart.h"

/*
 * DWC2 host driver.
 *
 * The controller has a set of "channels"; a transfer is set up by writing the device
 * address, endpoint, size and buffer into one channel and starting it, then waiting for
 * the channel's interrupt bits to say what happened. One channel is used here and
 * transfers are polled, which is slow but needs no interrupt plumbing to be correct.
 */

#define USB_BASE     (bcm_periph_base + 0x980000)
#define GOTGCTL      (USB_BASE + 0x000)
#define GAHBCFG      (USB_BASE + 0x008)
#define GUSBCFG      (USB_BASE + 0x00C)
#define GRSTCTL      (USB_BASE + 0x010)
#define GINTSTS      (USB_BASE + 0x014)
#define GINTMSK      (USB_BASE + 0x018)
#define GRXFSIZ      (USB_BASE + 0x024)
#define GNPTXFSIZ    (USB_BASE + 0x028)
#define GHWCFG2      (USB_BASE + 0x048)
#define HPTXFSIZ     (USB_BASE + 0x100)
#define HCFG         (USB_BASE + 0x400)
#define HPRT         (USB_BASE + 0x440)
#define HCCHAR(n)    (USB_BASE + 0x500 + (n) * 0x20)
#define HCINT(n)     (USB_BASE + 0x508 + (n) * 0x20)
#define HCINTMSK(n)  (USB_BASE + 0x50C + (n) * 0x20)
#define HCTSIZ(n)    (USB_BASE + 0x510 + (n) * 0x20)
#define HCDMA(n)     (USB_BASE + 0x514 + (n) * 0x20)

#define GRSTCTL_CSFTRST  (1u << 0)
#define GRSTCTL_AHBIDLE  (1u << 31)
#define GAHBCFG_DMAEN    (1u << 5)
#define GAHBCFG_GLBLINTR (1u << 0)

#define HPRT_CONNSTS  (1u << 0)
#define HPRT_CONNDET  (1u << 1)
#define HPRT_ENA      (1u << 2)
#define HPRT_ENACHG   (1u << 3)
#define HPRT_OVRCURR  (1u << 4)
#define HPRT_OVRCHG   (1u << 5)
#define HPRT_RST      (1u << 8)
#define HPRT_PWR      (1u << 12)
#define HPRT_SPD_SHIFT 17

#define HCCHAR_ENA    (1u << 31)
#define HCCHAR_DIS    (1u << 30)

#define HCINT_XFERCOMP (1u << 0)
#define HCINT_CHHLTD   (1u << 1)
#define HCINT_STALL    (1u << 3)
#define HCINT_NAK      (1u << 4)
#define HCINT_ACK      (1u << 5)
#define HCINT_XACTERR  (1u << 7)

/* standard requests */
#define REQ_GET_DESCRIPTOR 6
#define REQ_SET_ADDRESS    5
#define REQ_SET_CONFIG     9
#define DESC_DEVICE        1
#define DESC_CONFIG        2

static usb_device_t devices[USB_MAX_DEVICES];
static int ndevices;
static int next_address = 1;

/* The controller reads and writes memory behind the CPU's back, so every transfer goes
 * through buffers from the non-cacheable DMA region (dma.h) and is copied in or out. A
 * caller's buffer -- often on the stack -- would otherwise be half in the cache. */
#define XFER_MAX 2048
static uint8_t *dma_buf;
static uint8_t *setup_buf;
static void dma_buffers(void)
{
    if (!dma_buf) { dma_buf = dma_alloc(XFER_MAX); setup_buf = dma_alloc(64); }
}

/* Data toggle per device address, endpoint and direction: bulk packets alternate DATA0 and
 * DATA1, and a device silently drops one that repeats the toggle it last saw. */
static uint8_t toggle[16][16][2];

static void udelay(int us)
{
    /* no timer yet at this stage of the port: a calibrated-enough spin */
    bcm_delay(us * 100);
}

static int wait_bit(uint64_t reg, uint32_t mask, int set, int tries)
{
    for (int i = 0; i < tries; i++) {
        uint32_t v = mmio_read(reg);
        if (set ? (v & mask) : !(v & mask)) return 0;
        udelay(10);
    }
    return -1;
}

int dwc2_init(void)
{
    /* A core reset puts every register back to a known state; the AHB must be idle first
     * or the reset is ignored and everything after it reads as garbage. */
    if (wait_bit(GRSTCTL, GRSTCTL_AHBIDLE, 1, 10000) != 0) {
        uart_puts("usb: AHB never went idle\n");
        return -1;
    }
    mmio_write(GRSTCTL, GRSTCTL_CSFTRST);
    if (wait_bit(GRSTCTL, GRSTCTL_CSFTRST, 0, 10000) != 0) {
        uart_puts("usb: core reset did not finish\n");
        return -1;
    }
    udelay(100);

    /* host mode, internal DMA, FIFOs split the way the Pi's firmware expects */
    uint32_t cfg = mmio_read(GUSBCFG);
    cfg &= ~(1u << 30);                 /* not a device */
    cfg |= (1u << 29);                  /* force host mode */
    mmio_write(GUSBCFG, cfg);
    udelay(25000);

    mmio_write(GRXFSIZ, 0x306);
    mmio_write(GNPTXFSIZ, (0x100 << 16) | 0x306);
    mmio_write(HPTXFSIZ, (0x200 << 16) | 0x406);

    mmio_write(GAHBCFG, mmio_read(GAHBCFG) | GAHBCFG_DMAEN);
    mmio_write(HCFG, mmio_read(HCFG) & ~(3u << 0));   /* 30/60 MHz PHY clock */

    /* Power the port, then reset whatever is attached: a device only answers on address 0
     * after it has seen a bus reset. */
    uint32_t hprt = mmio_read(HPRT) & ~(HPRT_CONNDET | HPRT_ENA | HPRT_ENACHG | HPRT_OVRCHG);
    mmio_write(HPRT, hprt | HPRT_PWR);
    udelay(50000);
    return 0;
}

static int port_reset(void)
{
    uint32_t hprt = mmio_read(HPRT);
    if (!(hprt & HPRT_CONNSTS)) return -1;            /* nothing plugged in */
    hprt &= ~(HPRT_CONNDET | HPRT_ENA | HPRT_ENACHG | HPRT_OVRCHG);
    mmio_write(HPRT, hprt | HPRT_RST);
    udelay(60000);
    mmio_write(HPRT, hprt & ~HPRT_RST);
    udelay(60000);
    return 0;
}

/* The controller reaches memory through the same GPU-side window the mailbox uses, so a
 * buffer has to be handed over as a bus address, not the one the CPU sees. */
#define USB_BUS(addr) dma_gpu_bus(addr)

/* endpoint types as the channel register encodes them */
enum { EP_CONTROL = 0, EP_ISOC = 1, EP_BULK = 2, EP_INTERRUPT = 3 };

/* One transfer on channel 0. pid: 0 DATA0, 2 DATA1, 3 SETUP. */
static int channel_xfer(int addr, int ep, int in, int pid, void *buf, int len, int speed,
                        int maxp, int eptype)
{
    mmio_write(HCINT(0), 0xFFFFFFFF);                 /* clear anything stale */

    uint32_t chr = ((uint32_t)maxp & 0x7FF)
                 | ((uint32_t)(ep & 0xF) << 11)
                 | ((uint32_t)(in ? 1 : 0) << 15)
                 | ((speed == 2) ? (1u << 17) : 0)    /* low speed */
                 | ((uint32_t)eptype << 18)
                 | (1u << 20)                         /* one packet per frame */
                 | ((uint32_t)(addr & 0x7F) << 22);

    int packets = (len + maxp - 1) / maxp;
    if (packets == 0) packets = 1;
    dma_buffers();
    if (len > XFER_MAX || !dma_buf) return -1;
    if (buf != setup_buf && buf != dma_buf && !in && len) memcpy(dma_buf, buf, (size_t)len);
    void *dma = (buf == setup_buf) ? setup_buf : dma_buf;     /* dma_buf as a caller's buffer is already in place */
    mmio_write(HCTSIZ(0), ((uint32_t)pid << 29) | ((uint32_t)packets << 19) | (uint32_t)len);
    mmio_write(HCDMA(0), USB_BUS(dma));
    mmio_write(HCCHAR(0), chr | HCCHAR_ENA);

    for (int i = 0; i < 100000; i++) {
        uint32_t s = mmio_read(HCINT(0));
        if (s & HCINT_XFERCOMP) {
            mmio_write(HCINT(0), 0xFFFFFFFF);
            uint32_t left = mmio_read(HCTSIZ(0)) & 0x7FFFF;
            int got = len - (int)left;
            if (in && got > 0 && buf != setup_buf && buf != dma_buf) memcpy(buf, dma_buf, (size_t)got);
            return got;
        }
        if (s & (HCINT_STALL | HCINT_XACTERR)) {
            mmio_write(HCINT(0), 0xFFFFFFFF);
            return -1;
        }
        if (s & HCINT_CHHLTD) {
            mmio_write(HCINT(0), 0xFFFFFFFF);
            return -1;
        }
        udelay(5);
    }
    mmio_write(HCCHAR(0), mmio_read(HCCHAR(0)) | HCCHAR_DIS);
    return -1;
}

int usb_control(usb_device_t *dev, uint8_t rtype, uint8_t req, uint16_t value,
                uint16_t index, void *data, uint16_t len)
{
    int maxp = dev->speed == 2 ? 8 : 64;
    setup_buf[0] = rtype;
    setup_buf[1] = req;
    setup_buf[2] = (uint8_t)value;        setup_buf[3] = (uint8_t)(value >> 8);
    setup_buf[4] = (uint8_t)index;        setup_buf[5] = (uint8_t)(index >> 8);
    setup_buf[6] = (uint8_t)len;          setup_buf[7] = (uint8_t)(len >> 8);

    /* setup stage */
    if (channel_xfer(dev->address, 0, 0, 3, setup_buf, 8, dev->speed, maxp, EP_CONTROL) < 0) return -1;

    int got = 0;
    int in = (rtype & 0x80) != 0;
    if (len) {
        /* data stage; the first data packet after SETUP is always DATA1 */
        got = channel_xfer(dev->address, 0, in, 2, data, len, dev->speed, maxp, EP_CONTROL);
        if (got < 0) return -1;
    }
    /* status stage runs the other way round, with no data */
    if (channel_xfer(dev->address, 0, !in, 2, dma_buf, 0, dev->speed, maxp, EP_CONTROL) < 0) return -1;
    return got;
}

int usb_get_config(usb_device_t *dev, int index, uint8_t *buf, int max)
{
    uint8_t head[9];
    if (usb_control(dev, 0x80, REQ_GET_DESCRIPTOR, (uint16_t)((DESC_CONFIG << 8) | index), 0, head, 9) < 9) return -1;
    int total = head[2] | (head[3] << 8);
    if (total > max) total = max;
    if (total > XFER_MAX) total = XFER_MAX;
    return usb_control(dev, 0x80, REQ_GET_DESCRIPTOR, (uint16_t)((DESC_CONFIG << 8) | index), 0, buf, (uint16_t)total);
}

int usb_get_string(usb_device_t *dev, int index, char *out, int max)
{
    uint8_t b[128];
    int n = usb_control(dev, 0x80, REQ_GET_DESCRIPTOR, (uint16_t)((3 << 8) | index), 0x0409, b, sizeof(b));
    if (n < 2 || b[0] < 2) return -1;
    int len = (b[0] < n ? b[0] : n);
    int o = 0;
    for (int i = 2; i + 1 < len && o + 1 < max; i += 2) out[o++] = (char)b[i];   /* UTF-16LE, ASCII range */
    out[o] = 0;
    return o;
}

int usb_set_config(usb_device_t *dev, int value)
{
    return usb_control(dev, 0x00, REQ_SET_CONFIG, (uint16_t)value, 0, 0, 0) < 0 ? -1 : 0;
}

int usb_set_interface(usb_device_t *dev, int iface, int alt)
{
    return usb_control(dev, 0x01, 11 /* SET_INTERFACE */, (uint16_t)alt, (uint16_t)iface, 0, 0) < 0 ? -1 : 0;
}

int usb_bulk(usb_device_t *dev, uint8_t ep, void *data, int len)
{
    int in = (ep & 0x80) != 0;
    int maxp = dev->speed == 0 ? 512 : 64;           /* a high-speed bulk packet is 512 bytes */
    uint8_t *t = &toggle[dev->address & 15][ep & 15][in];
    int got = channel_xfer(dev->address, ep & 0xF, in, *t ? 2 : 0, data, len, dev->speed, maxp, EP_BULK);
    if (got >= 0) {
        int n = in ? got : len;
        int packets = (n + maxp - 1) / maxp;
        if (packets == 0) packets = 1;
        *t ^= (uint8_t)(packets & 1);
    }
    return got;
}

/*
 * Receiving without waiting. A network card has nothing to say most of the time, and a
 * bulk IN transfer to an idle device is answered with NAK; the blocking usb_bulk would sit
 * through its whole timeout on every poll. This keeps one IN transfer armed on channel 1 and
 * looks at it each time it is asked: a finished transfer is returned, a NAKed one is simply
 * started again, and a transfer still in flight costs nothing.
 */
static uint8_t *rx_dma;
static int rx_armed;
static usb_device_t *rx_dev;
static uint8_t rx_ep;

int usb_bulk_in_poll(usb_device_t *dev, uint8_t ep, void *data, int max)
{
    if (!rx_dma) rx_dma = dma_alloc(XFER_MAX);
    if (!rx_dma) return -1;
    int maxp = dev->speed == 0 ? 512 : 64;
    uint8_t *t = &toggle[dev->address & 15][ep & 15][1];

    if (rx_armed && (rx_dev != dev || rx_ep != ep)) rx_armed = 0;

    if (rx_armed) {
        uint32_t s = mmio_read(HCINT(1));
        if (s & HCINT_XFERCOMP) {
            uint32_t left = mmio_read(HCTSIZ(1)) & 0x7FFFF;
            mmio_write(HCINT(1), 0xFFFFFFFF);
            rx_armed = 0;
            int got = XFER_MAX - (int)left;
            if (got < 0) got = 0;
            int packets = (got + maxp - 1) / maxp;
            if (packets == 0) packets = 1;
            *t ^= (uint8_t)(packets & 1);
            if (got > max) got = max;
            memcpy(data, rx_dma, (size_t)got);
            return got;
        }
        if (s & (HCINT_STALL | HCINT_XACTERR)) {
            mmio_write(HCINT(1), 0xFFFFFFFF);
            rx_armed = 0;
            return -1;
        }
        if (s & HCINT_CHHLTD) {                 /* NAK: the device had nothing; ask again next time */
            mmio_write(HCINT(1), 0xFFFFFFFF);
            rx_armed = 0;
        }
        return 0;
    }

    mmio_write(HCINT(1), 0xFFFFFFFF);
    uint32_t chr = ((uint32_t)maxp & 0x7FF)
                 | ((uint32_t)(ep & 0xF) << 11)
                 | (1u << 15)                             /* IN */
                 | ((dev->speed == 2) ? (1u << 17) : 0)
                 | ((uint32_t)EP_BULK << 18)
                 | (1u << 20)
                 | ((uint32_t)(dev->address & 0x7F) << 22);
    int packets = XFER_MAX / maxp;
    mmio_write(HCTSIZ(1), ((uint32_t)(*t ? 2 : 0) << 29) | ((uint32_t)packets << 19) | (uint32_t)XFER_MAX);
    mmio_write(HCDMA(1), USB_BUS(rx_dma));
    mmio_write(HCCHAR(1), chr | HCCHAR_ENA);
    rx_armed = 1;
    rx_dev = dev;
    rx_ep = ep;
    return 0;
}

/* ---- hubs ----
 *
 * Every Pi has a hub soldered onto the board (on a Pi 3 it is inside the LAN9514, with the
 * Ethernet behind it), so without walking the hub's ports the only device ever found is
 * the hub itself.
 */

#define HUB_GET_STATUS   0
#define HUB_SET_FEATURE  3
#define HUB_CLEAR_FEATURE 1
#define PORT_CONNECTION  0
#define PORT_ENABLE      1
#define PORT_RESET       4
#define PORT_POWER       8
#define C_PORT_CONNECTION 16
#define C_PORT_RESET     20

/* Brings up one device: assigns it an address and reads what it is. */
static int device_setup(usb_device_t *d, int speed, int port)
{
    d->used = 1;
    d->address = 0;
    d->speed = (uint8_t)speed;
    d->hub_port = port;

    if (usb_control(d, 0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, dma_buf, 8) < 0) {
        d->used = 0;
        return -1;
    }
    uint8_t addr = (uint8_t)next_address++;
    if (usb_control(d, 0x00, REQ_SET_ADDRESS, addr, 0, 0, 0) < 0) { d->used = 0; return -1; }
    d->address = addr;
    udelay(10000);

    if (usb_control(d, 0x80, REQ_GET_DESCRIPTOR, (DESC_DEVICE << 8), 0, dma_buf, 18) < 0) {
        d->used = 0;
        return -1;
    }
    d->dev_class    = dma_buf[4];
    d->dev_subclass = dma_buf[5];
    d->dev_protocol = dma_buf[6];
    d->vendor  = (uint16_t)(dma_buf[8]  | (dma_buf[9]  << 8));
    d->product = (uint16_t)(dma_buf[10] | (dma_buf[11] << 8));

    if (usb_control(d, 0x80, REQ_GET_DESCRIPTOR, (DESC_CONFIG << 8), 0, dma_buf, 9) >= 0) {
        d->config_value = dma_buf[5];
        usb_control(d, 0x00, REQ_SET_CONFIG, d->config_value, 0, 0, 0);
    }
    return 0;
}

/* Walks a hub's ports and brings up whatever is plugged into each one. */
static void hub_scan(usb_device_t *hub)
{
    uint8_t desc[16];
    /* class-specific GET_DESCRIPTOR for the hub descriptor (type 0x29) */
    if (usb_control(hub, 0xA0, REQ_GET_DESCRIPTOR, (0x29 << 8), 0, desc, 9) < 0) {
        uart_puts("usb: hub descriptor failed\n");
        return;
    }
    int ports = desc[2];
    if (ports > 15) ports = 15;

    for (int p = 1; p <= ports; p++)
        usb_control(hub, 0x23, HUB_SET_FEATURE, PORT_POWER, (uint16_t)p, 0, 0);
    udelay(100000);                          /* the hub needs time to power its ports */

    for (int p = 1; p <= ports && ndevices < USB_MAX_DEVICES; p++) {
        uint8_t st[4];
        if (usb_control(hub, 0xA3, HUB_GET_STATUS, 0, (uint16_t)p, st, 4) < 0) continue;
        uint16_t status = (uint16_t)(st[0] | (st[1] << 8));
        if (!(status & (1 << PORT_CONNECTION))) continue;

        usb_control(hub, 0x23, HUB_SET_FEATURE, PORT_RESET, (uint16_t)p, 0, 0);
        udelay(60000);
        usb_control(hub, 0x23, HUB_CLEAR_FEATURE, C_PORT_RESET, (uint16_t)p, 0, 0);
        udelay(20000);

        if (usb_control(hub, 0xA3, HUB_GET_STATUS, 0, (uint16_t)p, st, 4) < 0) continue;
        status = (uint16_t)(st[0] | (st[1] << 8));
        if (!(status & (1 << PORT_ENABLE))) continue;
        /* bit 9 low speed, bit 10 high speed; neither means full speed */
        int speed = (status & (1 << 9)) ? 2 : (status & (1 << 10)) ? 0 : 1;

        usb_device_t *d = &devices[ndevices];
        if (device_setup(d, speed, p) == 0) ndevices++;
    }
}

int usb_enumerate(void)
{
    ndevices = 0;
    next_address = 1;
    memset(toggle, 0, sizeof(toggle));
    for (int i = 0; i < USB_MAX_DEVICES; i++) devices[i].used = 0;

    uint32_t hprt = mmio_read(HPRT);
    if (!(hprt & HPRT_CONNSTS)) {
        uart_puts("usb: nothing attached to the root port\n");
        return 0;
    }
    if (port_reset() != 0) return 0;

    hprt = mmio_read(HPRT);
    int speed = (int)((hprt >> HPRT_SPD_SHIFT) & 3);

    usb_device_t *root = &devices[0];
    if (device_setup(root, speed, 0) != 0) {
        uart_puts("usb: the root device did not answer\n");
        return 0;
    }
    ndevices = 1;

    /* A hub here is the normal case on a Pi: everything else is behind it. */
    if (root->dev_class == 9) hub_scan(root);
    return ndevices;
}

int usb_device_count(void) { return ndevices; }

const usb_device_t *usb_device_get(int i)
{
    if (i < 0 || i >= ndevices) return 0;
    return &devices[i];
}
