#include "ethdev.h"
#include "dwc2.h"
#include "uart.h"
#include "irq.h"
#include "string_arm.h"

/*
 * The SMSC LAN9512/9514, the Ethernet on a Pi 2 and 3.
 *
 * It is a USB device, not a memory-mapped block: its registers are read and written with
 * vendor control transfers, and frames go over bulk endpoints with a small header of their
 * own. The hub the Pi exposes is inside this same chip.
 */

#define LAN_VENDOR 0x0424

/* registers, reached through vendor requests 0xA0 (read) and 0xA1 (write) */
#define ID_REV        0x000
#define INT_STS       0x008
#define RX_CFG        0x00C
#define TX_CFG        0x010
#define HW_CFG        0x014
#define MAC_CR        0x100
#define ADDRH         0x104
#define ADDRL         0x108
#define MII_ADDR      0x114
#define MII_DATA      0x118

#define HW_CFG_LRST   (1u << 3)
#define HW_CFG_BIR    (1u << 12)       /* bulk-in uses its own buffer rounding */
#define TX_CFG_ON     (1u << 2)
#define MAC_CR_RXEN   (1u << 2)
#define MAC_CR_TXEN   (1u << 3)
#define MAC_CR_FDPX   (1u << 20)

static usb_device_t *dev;
static uint8_t our_mac[6];
static uint8_t ep_in, ep_out;
static int started;

static int reg_write(uint16_t reg, uint32_t value)
{
    uint32_t v = value;
    return usb_control(dev, 0x40, 0xA0, 0, reg, &v, 4) < 0 ? -1 : 0;
}

static int reg_read(uint16_t reg, uint32_t *value)
{
    return usb_control(dev, 0xC0, 0xA1, 0, reg, value, 4) < 0 ? -1 : 0;
}

static int lan_start(void)
{
    /* Find the chip among what enumeration turned up. */
    dev = 0;
    for (int i = 0; i < usb_device_count(); i++) {
        const usb_device_t *d = usb_device_get(i);
        if (d && d->vendor == LAN_VENDOR) { dev = (usb_device_t *)d; break; }
    }
    if (!dev) return -1;

    uint32_t id = 0;
    if (reg_read(ID_REV, &id) != 0 || id == 0) return -1;

    /* A chip-level reset, then wait for it to release. */
    if (reg_write(HW_CFG, HW_CFG_LRST) != 0) return -1;
    for (int i = 0; i < 1000; i++) {
        uint32_t hw = 0;
        if (reg_read(HW_CFG, &hw) == 0 && !(hw & HW_CFG_LRST)) break;
        timer_delay_ms(1);
    }

    uint32_t hi = 0, lo = 0;
    if (reg_read(ADDRH, &hi) != 0 || reg_read(ADDRL, &lo) != 0) return -1;
    our_mac[0] = (uint8_t)lo;         our_mac[1] = (uint8_t)(lo >> 8);
    our_mac[2] = (uint8_t)(lo >> 16); our_mac[3] = (uint8_t)(lo >> 24);
    our_mac[4] = (uint8_t)hi;         our_mac[5] = (uint8_t)(hi >> 8);

    reg_write(HW_CFG, HW_CFG_BIR);
    reg_write(TX_CFG, TX_CFG_ON);
    reg_write(MAC_CR, MAC_CR_RXEN | MAC_CR_TXEN | MAC_CR_FDPX);

    /* The data endpoints are the first bulk pair of the only interface this chip has. */
    ep_in = 0x81;
    ep_out = 0x02;
    started = 1;
    return 0;
}

static int lan_send(const uint8_t *frame, int len)
{
    if (!started || len <= 0 || len > ETH_MTU) return -1;
    /* Two words in front of the frame: what to do with it, and how long it is. */
    static uint8_t out[ETH_MTU + 8] __attribute__((aligned(8)));
    uint32_t cmd_a = (uint32_t)len | (1u << 12) | (1u << 13);   /* first and last segment */
    uint32_t cmd_b = (uint32_t)len;
    memcpy(out + 0, &cmd_a, 4);
    memcpy(out + 4, &cmd_b, 4);
    memcpy(out + 8, frame, (size_t)len);
    return usb_bulk(dev, ep_out, out, len + 8) < 0 ? -1 : len;
}

static int lan_recv(uint8_t *frame, int max)
{
    if (!started) return 0;
    static uint8_t in[ETH_MTU + 8] __attribute__((aligned(8)));
    int n = usb_bulk_in_poll(dev, ep_in, in, (int)sizeof(in));
    if (n <= 8) return 0;
    /* A four-byte status word leads each frame; the length lives in its upper bits. */
    uint32_t status;
    memcpy(&status, in, 4);
    int len = (int)((status >> 16) & 0x3FFF);
    if (len <= 0 || len > n - 4) return 0;
    if (len > max) len = max;
    memcpy(frame, in + 4, (size_t)len);
    return len;
}

eth_driver_t lan95xx_driver = { "LAN95xx over USB (Pi 2/3)", lan_start, lan_send, lan_recv, {0} };

void lan95xx_publish_mac(uint8_t *out) { memcpy(out, our_mac, 6); }
