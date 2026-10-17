#include "uhci.h"
#include "pci.h"
#include "io.h"
#include "memory.h"
#include "string.h"
#include "klog.h"

#define FRAME_NUM 1024

unsigned char _uhci_dma[65536] __attribute__((aligned(4096)));
static uint32_t _dma_pos;
static uint32_t _dma_base;        /* past the frame list and queue head, which stay for good */

static uhci_controller_t _saved_ctrl;
static int _uhci_ready = 0;

static void *dma_zalloc(uint32_t sz, uint32_t *phys)
{
    _dma_pos = (_dma_pos + 15) & ~15;
    /* transfer descriptors are only alive for one transfer at a time: once the area is used up,
     * start again from the beginning instead of running off its end */
    if (_dma_pos + ((sz + 15) & ~15) > sizeof(_uhci_dma)) _dma_pos = _dma_base;
    *phys = (uint32_t)(_uhci_dma + _dma_pos);
    void *p = _uhci_dma + _dma_pos;
    _dma_pos += (sz + 15) & ~15;
    memset(p, 0, (sz + 15) & ~15);
    return p;
}

static void klog_hex(uint32_t v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    klog_write(b);
}

#define r16(c, r) inw((c)->io_base + (r))
#define w16(c, r, v) outw((c)->io_base + (r), (v))
#define r32(c, r) inl((c)->io_base + (r))
#define w32(c, r, v) outl((c)->io_base + (r), (v))

int uhci_init(uhci_controller_t *c)
{
    if (_uhci_ready) {
        *c = _saved_ctrl;
        return 0;
    }
    memset(c, 0, sizeof(*c));
    _dma_pos = 0;

    pci_device_t devs[4];
    int n = pci_find_devices(0x0C, 0x03, devs, 4);
    /* USB controllers share the class: UHCI is programming interface 0x00 (OHCI 0x10, EHCI 0x20,
     * xHCI 0x30), and with -usb plus an EHCI the UHCI is not necessarily the first one */
    int pick = -1;
    for (int i = 0; i < n && pick < 0; i++) if (devs[i].prog_if == 0x00) pick = i;
    if (pick < 0) { klog_write("uhci: no UHCI controller on the PCI bus\n"); return -1; }

    c->pci = devs[pick];
    uint32_t bar4 = pci_get_bar(c->pci.bus, c->pci.device, c->pci.func, 4);
    c->io_base = bar4 & 0xFFF0;

    pci_write_config(c->pci.bus, c->pci.device, c->pci.func, 4, 5 | (1u << 10));

    w16(c, 0, 2);
    volatile int t = 50000;
    while (r16(c, 0) & 2 && --t);

    uint32_t flp;
    c->frame_list = dma_zalloc(FRAME_NUM * 4, &flp);
    c->frame_list_phys = flp;

    uint32_t qp;
    c->async_qh = dma_zalloc(32, &qp);

    c->async_qh->link = 1;
    c->async_qh->element = 1;

    for (int i = 0; i < FRAME_NUM; i++)
        c->frame_list[i] = qp | 2;

    w32(c, 8, flp);
    w16(c, 12, 0x40);
    w16(c, 4, 0);
    w16(c, 0, 1);

    t = 50000;
    while (r16(c, 2) & 0x20 && --t);

    if (!t) {
        klog_write("uhci: controller did not start, io ");
        klog_hex(c->io_base);
        klog_write(" status ");
        klog_hex(r16(c, 2));
        klog_write("\n");
        return -1;
    }
    _dma_base = _dma_pos;
    _saved_ctrl = *c;
    _uhci_ready = 1;
    return 0;
}

/* is something plugged into the root port? (no reset, just the connect status bit) */
int uhci_port_connected(uhci_controller_t *c, int port)
{
    uint16_t s = r16(c, 16 + port * 2);
    if (s & 2) w16(c, 16 + port * 2, (uint16_t)(s & ~(4 | 8)) | 2);   /* acknowledge the change */
    return s & 1;
}

int uhci_port_detect(uhci_controller_t *c, int port)
{
    int re = 16 + port * 2;
    uint16_t s = r16(c, re);
    if (!(s & 1)) return 0;

    w16(c, re, 0x200);
    for (volatile int d = 0; d < 80000; d++);
    w16(c, re, 0);
    for (volatile int d = 0; d < 80000; d++);

    s = r16(c, re);
    if (s & 2) w16(c, re, s | 2);
    if (!(s & 4)) w16(c, re, s | 4);

    return 1;
}

static inline uint32_t uhci_link_td(void *td)
{
    return (uint32_t)td & 0xFFFFFFF0;
}

#define UHCI_PID_SETUP 0x2D
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xE1

#define UHCI_TD_ACTIVE      0x00800000
#define UHCI_TD_ERROR_MASK  0x007E0000
#define UHCI_TD_CERR3       0x18000000
#define UHCI_TD_INIT_STATUS (UHCI_TD_ACTIVE | UHCI_TD_CERR3)

static inline uint32_t uhci_token(uint32_t pid, int dev, int ep, int toggle, int len)
{
    uint32_t maxlen = ((uint32_t)(len - 1)) & 0x7FF;
    return pid | ((uint32_t)(dev & 0x7F) << 8) | ((uint32_t)(ep & 0x0F) << 15) |
           ((uint32_t)(toggle & 1) << 19) | (maxlen << 21);
}

int uhci_control(uhci_controller_t *c, int dev, int ep, usb_device_request_t *req, void *data, int dir)
{
    uint32_t tdp;
    uhci_td_t *td = (uhci_td_t *)dma_zalloc(3 * sizeof(uhci_td_t), &tdp);

    int has_data = data && req->wLength != 0;
    int status_idx = has_data ? 2 : 1;

    td[0].link = uhci_link_td(&td[1]);
    td[0].status = UHCI_TD_INIT_STATUS;
    td[0].token = uhci_token(UHCI_PID_SETUP, dev, ep, 0, 8);
    td[0].buffer = (uint32_t)req;

    if (has_data) {
        td[1].link = uhci_link_td(&td[2]);
        td[1].status = UHCI_TD_INIT_STATUS;
        td[1].token = uhci_token((dir & 0x80) ? UHCI_PID_IN : UHCI_PID_OUT, dev, ep, 1, req->wLength);
        td[1].buffer = (uint32_t)data;
    }

    td[status_idx].link = 1;
    td[status_idx].status = UHCI_TD_INIT_STATUS;
    td[status_idx].buffer = 0;
    td[status_idx].token = uhci_token((dir & 0x80) ? UHCI_PID_OUT : UHCI_PID_IN, dev, ep, 1, 0);

    c->async_qh->element = (uint32_t)&td[0];

    int t = 2000000;
    while (--t) {
        inb(0x80);
        if (!(td[status_idx].status & UHCI_TD_ACTIVE)) break;
    }

    c->async_qh->element = 1;

    return (!t || (td[status_idx].status & UHCI_TD_ACTIVE)) ? -1 : 0;
}

int uhci_interrupt_read(uhci_controller_t *c, int dev, int ep, int max_len, void *buf)
{
    uint32_t tdp;
    uhci_td_t *td = (uhci_td_t *)dma_zalloc(sizeof(uhci_td_t), &tdp);

    td->link = 1;
    td->status = UHCI_TD_INIT_STATUS;
    td->token = uhci_token(UHCI_PID_IN, dev, ep, 0, max_len);
    td->buffer = (uint32_t)buf;

    c->async_qh->element = (uint32_t)td;

    int t = 500000;
    while (--t) {
        inb(0x80);
        if (!(td->status & UHCI_TD_ACTIVE)) break;
    }

    c->async_qh->element = 1;

    if (td->status & UHCI_TD_ACTIVE) return -1;
    if (td->status & UHCI_TD_ERROR_MASK) return -1;
    return 0;
}

int uhci_bulk_transfer(uhci_controller_t *c, int dev, int ep, int dir, void *buf, int len, int maxpacket, int *toggle)
{
    uint8_t *p = (uint8_t *)buf;
    int remaining = len;

    if (maxpacket <= 0) maxpacket = 64;

    while (remaining > 0 || len == 0) {
        int chunk = remaining > maxpacket ? maxpacket : remaining;

        uint32_t tdp;
        uhci_td_t *td = (uhci_td_t *)dma_zalloc(sizeof(uhci_td_t), &tdp);

        td->link = 1;
        td->status = UHCI_TD_INIT_STATUS;
        td->token = uhci_token(dir ? UHCI_PID_IN : UHCI_PID_OUT, dev, ep, *toggle, chunk);
        td->buffer = (uint32_t)p;

        c->async_qh->element = (uint32_t)td;

        int t = 2000000;
        while (--t) {
            inb(0x80);
            if (!(td->status & UHCI_TD_ACTIVE)) break;
        }

        c->async_qh->element = 1;

        if (!t) return -1;
        if (td->status & UHCI_TD_ERROR_MASK) return -1;

        *toggle ^= 1;
        p += chunk;
        remaining -= chunk;
        if (len == 0) break;
    }

    return 0;
}
