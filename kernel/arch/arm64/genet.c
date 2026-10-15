#include "ethdev.h"
#include "bcm.h"
#include "dma.h"
#include "mailbox.h"
#include "uart.h"
#include "irq.h"
#include "string_arm.h"

/*
 * GENET v5, the Ethernet controller built into the Pi 4's SoC (BCM2711).
 *
 * The register map and the order things have to be done in follow what Linux's bcmgenet
 * driver does on this very chip; every offset below was checked against it, not recalled.
 * It could not be run against the real controller here -- QEMU does not model it -- so the
 * first start on a real board prints what it finds, which is what to look at if it fails.
 *
 * Shape of the hardware:
 *   - a block of registers at 0xFD580000 (below the "legacy" peripherals at 0xFE000000);
 *   - one receive and one transmit DMA engine, each with its descriptors held in on-chip
 *     RAM that is reached through the register window (12 bytes each: length/status, address
 *     low, address high), and a set of rings over those descriptors -- ring 16 is the default
 *     one and the only one used here;
 *   - a UniMAC, and an external Broadcom gigabit PHY (BCM54213PE) on MDIO address 1, wired
 *     over RGMII with the receive clock delayed by the PHY ("rgmii-rxid" in Linux's tree).
 *
 * Frames are copied to and from buffers in the non-cacheable DMA region, so there is no cache
 * maintenance anywhere in here.
 */

#define GENET_BASE      0xFD580000ull

/* system block */
#define SYS_REV_CTRL    0x000
#define SYS_PORT_CTRL   0x004
#define   PORT_MODE_EXT_GPHY   3
#define SYS_RBUF_FLUSH  0x008
#define EXT_RGMII_OOB   0x08C
#define   RGMII_LINK      (1u << 4)
#define   OOB_DISABLE     (1u << 5)
#define   RGMII_MODE_EN   (1u << 6)
#define   ID_MODE_DIS     (1u << 16)

/* interrupt controllers: everything is polled, so everything is masked */
#define INTRL2_0        0x200
#define INTRL2_1        0x240
#define INTRL2_MASK_SET   0x10
#define INTRL2_CLEAR      0x08

#define RBUF_CTRL       0x300
#define RBUF_CHK_CTRL   0x314
#define RBUF_TBUF_SIZE  0x3B4
#define TBUF_CTRL       0x600

/* UniMAC */
#define UMAC_CMD        0x808
#define UMAC_MAC0       0x80C
#define UMAC_MAC1       0x810
#define UMAC_MAX_FRAME  0x814
#define UMAC_MIB_CTRL   0xD80
#define UMAC_TX_FLUSH   0xB34
#define UMAC_MDIO_CMD   0xE14

#define CMD_TX_EN            (1u << 0)
#define CMD_RX_EN            (1u << 1)
#define CMD_SPEED_SHIFT      2
#define CMD_PROMISC          (1u << 4)
#define CMD_RX_PAUSE_IGNORE  (1u << 8)
#define CMD_HD_EN            (1u << 10)
#define CMD_SW_RESET         (1u << 13)
#define CMD_TX_PAUSE_IGNORE  (1u << 28)

#define MDIO_START_BUSY (1u << 29)
#define MDIO_READ_FAIL  (1u << 28)
#define MDIO_RD         (2u << 26)
#define MDIO_WR         (1u << 26)

/* DMA: descriptors first (256 of them), then the registers */
#define RDMA_DESC       0x2000
#define TDMA_DESC       0x4000
#define DESC_SIZE       12
#define DMA_REGS(d)     ((d) + 256 * DESC_SIZE)
#define RING16(d)       (DMA_REGS(d) + 16 * 0x40)         /* the default ring's registers */
#define GLOBAL(d)       (DMA_REGS(d) + 17 * 0x40)         /* ring-independent registers */

/* per-ring registers (GENET v4/v5 layout) */
#define R_PTR_A         0x00      /* tx read pointer / rx write pointer */
#define R_IDX_A         0x08      /* tx consumer index / rx producer index */
#define R_IDX_B         0x0C      /* tx producer index / rx consumer index */
#define R_BUF_SIZE      0x10
#define R_START         0x14
#define R_END           0x1C
#define R_DONE_THRESH   0x24
#define R_FLOW          0x28      /* tx flow period / rx xon-xoff thresholds */
#define R_PTR_B         0x2C      /* tx write pointer / rx read pointer */

/* global registers */
#define G_RING_CFG      0x00
#define G_CTRL          0x04
#define G_STATUS        0x08
#define G_BURST         0x0C
#define G_ARB           0x2C
#define G_PRIO2         0x38

#define DMA_EN          (1u << 0)
#define DMA_RING16_EN   (1u << 17)
#define DMA_DISABLED    (1u << 0)

/* descriptor length/status word */
#define D_OWN           0x8000
#define D_EOP           0x4000
#define D_SOP           0x2000
#define D_APPEND_CRC    0x0040
#define D_QTAG          (0x3Fu << 7)
#define D_RX_ERRORS     0x001F    /* overrun, CRC, receive error, alignment, too long */

#define NDESC           64
#define BUF_SIZE        2048
#define WORDS_PER_BD    3

#define PHY_ADDR        1         /* the Pi 4's PHY, per the board's device tree */

static uint8_t *rx_bufs, *tx_bufs;
static uint32_t rx_consumer, tx_producer;
static uint8_t our_mac[6];
static int started, linked;
static uint64_t last_link_check;
static int phy_addr = PHY_ADDR;

static inline uint32_t rd(uint32_t off)            { return mmio_read(GENET_BASE + off); }
static inline void     wr(uint32_t off, uint32_t v) { mmio_write(GENET_BASE + off, v); }

static int mdio_wait(void)
{
    for (int i = 0; i < 2000; i++) {
        if (!(rd(UMAC_MDIO_CMD) & MDIO_START_BUSY)) return 0;
        timer_delay_us(5);
    }
    return -1;
}

static int mdio_read(int phy, int reg)
{
    wr(UMAC_MDIO_CMD, MDIO_START_BUSY | MDIO_RD | ((uint32_t)phy << 21) | ((uint32_t)reg << 16));
    if (mdio_wait() != 0) return -1;
    uint32_t v = rd(UMAC_MDIO_CMD);
    if (v & MDIO_READ_FAIL) return -1;
    return (int)(v & 0xFFFF);
}

static void mdio_write(int phy, int reg, int value)
{
    wr(UMAC_MDIO_CMD, MDIO_START_BUSY | MDIO_WR | ((uint32_t)phy << 21) | ((uint32_t)reg << 16) |
                      ((uint32_t)value & 0xFFFF));
    mdio_wait();
}

/* standard MII registers */
#define MII_BMCR     0
#define MII_BMSR     1
#define MII_PHYID1   2
#define MII_ADVERT   4
#define MII_LPA      5
#define MII_CTRL1000 9
#define MII_STAT1000 10

static void dma_stop(uint32_t desc)
{
    uint32_t g = GLOBAL(desc);
    wr(g + G_CTRL, rd(g + G_CTRL) & ~(DMA_EN | DMA_RING16_EN));
    for (int i = 0; i < 5000; i++) {
        if (rd(g + G_STATUS) & DMA_DISABLED) break;
        timer_delay_us(1);
    }
}

static void ring_setup(uint32_t desc, int rx)
{
    uint32_t r = RING16(desc);
    wr(r + R_IDX_A, 0);
    wr(r + R_IDX_B, 0);
    wr(r + R_BUF_SIZE, ((uint32_t)NDESC << 16) | BUF_SIZE);
    if (rx) wr(r + R_FLOW, (5u << 16) | (256 >> 4));          /* xon/xoff thresholds, as Linux sets them */
    else    { wr(r + R_DONE_THRESH, 10); wr(r + R_FLOW, 0); }  /* tx: no rate limiting on the default ring */
    wr(r + R_START, 0);
    wr(r + R_PTR_A, 0);
    wr(r + R_PTR_B, 0);
    wr(r + R_END, NDESC * WORDS_PER_BD - 1);
}

static void dma_init(void)
{
    uint32_t rg = GLOBAL(RDMA_DESC), tg = GLOBAL(TDMA_DESC);

    dma_stop(RDMA_DESC);
    dma_stop(TDMA_DESC);

    wr(UMAC_TX_FLUSH, 1);
    timer_delay_us(10);
    wr(UMAC_TX_FLUSH, 0);

    uint32_t f = rd(SYS_RBUF_FLUSH);
    wr(SYS_RBUF_FLUSH, f | 1);
    timer_delay_us(10);
    wr(SYS_RBUF_FLUSH, f);
    timer_delay_us(10);

    /* the receive descriptors: each one only needs the address of a buffer, the controller
     * fills in the length and flags */
    for (int i = 0; i < NDESC; i++) {
        uint64_t buf = (uint64_t)(uintptr_t)(rx_bufs + i * BUF_SIZE);
        wr(RDMA_DESC + i * DESC_SIZE + 4, (uint32_t)buf);
        wr(RDMA_DESC + i * DESC_SIZE + 8, (uint32_t)(buf >> 32));
    }

    wr(rg + G_BURST, 8);                       /* the BCM2711's burst length */
    wr(tg + G_BURST, 8);
    ring_setup(RDMA_DESC, 1);
    ring_setup(TDMA_DESC, 0);

    wr(tg + G_ARB, 2);                         /* strict priority between rings; only one is in use */
    wr(tg + G_PRIO2, 4u << 20);
    wr(rg + G_RING_CFG, 1u << 16);
    wr(tg + G_RING_CFG, 1u << 16);

    rx_consumer = 0;
    tx_producer = 0;
    wr(rg + G_CTRL, DMA_RING16_EN | DMA_EN);
    wr(tg + G_CTRL, DMA_RING16_EN | DMA_EN);
}

static void mac_program(void)
{
    wr(UMAC_MAC0, ((uint32_t)our_mac[0] << 24) | ((uint32_t)our_mac[1] << 16) |
                  ((uint32_t)our_mac[2] << 8) | our_mac[3]);
    wr(UMAC_MAC1, ((uint32_t)our_mac[4] << 8) | our_mac[5]);
}

static void mac_choose(void)
{
    /* The firmware knows the address burned into the board; the controller's own registers
     * come out of reset empty. */
    if (mailbox_mac_address(our_mac) != 0 || (our_mac[0] & 1) ||
        !(our_mac[0] | our_mac[1] | our_mac[2] | our_mac[3] | our_mac[4] | our_mac[5])) {
        static const uint8_t fallback[6] = { 0x02, 0x41, 0x52, 0x4D, 0x74, 0x4F };
        memcpy(our_mac, fallback, 6);
        uart_puts("genet: the firmware gave no MAC address, using a made-up one\n");
    }
}

/* Reads the PHY; when a link has come up, gives the MAC the speed and duplex that was agreed. */
static void link_update(void)
{
    uint64_t now = timer_us();
    if (now - last_link_check < (linked ? 1000000 : 250000)) return;
    last_link_check = now;

    int bmsr = mdio_read(phy_addr, MII_BMSR);
    if (bmsr >= 0) bmsr = mdio_read(phy_addr, MII_BMSR);       /* the link bit latches low */
    int up = bmsr >= 0 && (bmsr & 0x0004);

    if (!up) {
        if (linked) {
            linked = 0;
            wr(EXT_RGMII_OOB, rd(EXT_RGMII_OOB) & ~RGMII_LINK);
            uart_puts("genet: link down\n");
        }
        return;
    }
    if (linked) return;

    int adv = mdio_read(phy_addr, MII_ADVERT), lpa = mdio_read(phy_addr, MII_LPA);
    int c1000 = mdio_read(phy_addr, MII_CTRL1000), s1000 = mdio_read(phy_addr, MII_STAT1000);
    int speed = 10, full = 0;
    if (c1000 >= 0 && s1000 >= 0 && (c1000 & 0x0200) && (s1000 & 0x0800))       { speed = 1000; full = 1; }
    else if (c1000 >= 0 && s1000 >= 0 && (c1000 & 0x0100) && (s1000 & 0x0400))  { speed = 1000; full = 0; }
    else if (adv >= 0 && lpa >= 0) {
        int common = adv & lpa;
        if (common & 0x0100)      { speed = 100; full = 1; }
        else if (common & 0x0080) { speed = 100; full = 0; }
        else if (common & 0x0040) { speed = 10;  full = 1; }
    }

    /* The speed in the command register is also what tells the RGMII block which transmit
     * clock to use. The MAC is held in reset while it is changed, then released. */
    uint32_t sp = speed == 1000 ? 2 : speed == 100 ? 1 : 0;
    uint32_t cmd = CMD_SW_RESET | CMD_PROMISC | CMD_RX_PAUSE_IGNORE | CMD_TX_PAUSE_IGNORE | (sp << CMD_SPEED_SHIFT);
    if (!full) cmd |= CMD_HD_EN;
    wr(EXT_RGMII_OOB, rd(EXT_RGMII_OOB) | RGMII_LINK);
    wr(UMAC_CMD, cmd);
    timer_delay_us(2);
    cmd &= ~CMD_SW_RESET;
    wr(UMAC_CMD, cmd);
    timer_delay_us(2);
    wr(UMAC_CMD, cmd | CMD_TX_EN | CMD_RX_EN);

    linked = 1;
    uart_puts("genet: link up at ");
    uart_put_dec((uint64_t)speed);
    uart_puts(full ? " Mbit/s full duplex\n" : " Mbit/s half duplex\n");
}

static int phy_start(void)
{
    /* Find the PHY: it is at address 1 on a Pi 4, but a wrong guess costs one scan. */
    int found = -1;
    for (int pass = 0; pass < 2 && found < 0; pass++) {
        for (int a = 0; a < 32; a++) {
            int addr = pass == 0 ? PHY_ADDR : a;
            int id = mdio_read(addr, MII_PHYID1);
            if (id > 0 && id != 0xFFFF) { found = addr; break; }
            if (pass == 0) break;
        }
    }
    if (found < 0) { uart_puts("genet: no PHY answers on the MDIO bus\n"); return -1; }
    phy_addr = found;

    mdio_write(phy_addr, MII_BMCR, 0x8000);                    /* reset */
    for (int i = 0; i < 100; i++) {
        int v = mdio_read(phy_addr, MII_BMCR);
        if (v >= 0 && !(v & 0x8000)) break;
        timer_delay_ms(1);
    }
    mdio_write(phy_addr, MII_ADVERT, 0x01E1);                  /* 10/100, both duplex */
    mdio_write(phy_addr, MII_CTRL1000, 0x0200);                /* 1000 full */
    mdio_write(phy_addr, MII_BMCR, 0x1200);                    /* autonegotiate, restart */
    return 0;
}

static int genet_start(void)
{
    if (bcm_generation != 4) return -1;           /* only the BCM2711 has this block */

    uint32_t rev = 0;
    if (mmio_probe(GENET_BASE + SYS_REV_CTRL, &rev) != 0) return -1;     /* an emulator without the block */
    if (rev == 0xFFFFFFFF || rev == 0) return -1;
    uart_puts("genet: revision register ");
    uart_put_hex(rev);
    uart_puts("\n");

    if (!rx_bufs) {
        rx_bufs = dma_alloc(NDESC * BUF_SIZE);
        tx_bufs = dma_alloc(NDESC * BUF_SIZE);
        if (!rx_bufs || !tx_bufs) { uart_puts("genet: no DMA memory\n"); return -1; }
    }

    /* Hold the MAC in reset while everything around it is configured. */
    wr(SYS_RBUF_FLUSH, 0);
    timer_delay_us(10);
    wr(UMAC_CMD, CMD_SW_RESET);
    timer_delay_us(10);

    /* clear the counters */
    wr(UMAC_MIB_CTRL, 7);
    wr(UMAC_MIB_CTRL, 0);
    wr(UMAC_MAX_FRAME, ETH_MTU);

    /* no 64-byte status blocks and no alignment padding: frames arrive as they are, with the
     * length and flags in the descriptor */
    wr(TBUF_CTRL, rd(TBUF_CTRL) & ~1u);
    wr(RBUF_CTRL, rd(RBUF_CTRL) & ~3u);
    wr(RBUF_CHK_CTRL, 0);
    wr(RBUF_TBUF_SIZE, 1);

    /* everything is polled */
    wr(INTRL2_0 + INTRL2_MASK_SET, 0xFFFFFFFF);
    wr(INTRL2_0 + INTRL2_CLEAR, 0xFFFFFFFF);
    wr(INTRL2_1 + INTRL2_MASK_SET, 0xFFFFFFFF);
    wr(INTRL2_1 + INTRL2_CLEAR, 0xFFFFFFFF);

    mac_choose();
    mac_program();

    /* external gigabit PHY over RGMII; the PHY delays the receive clock, so the MAC's own
     * delay stays enabled (ID_MODE_DIS clear) */
    wr(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);
    uint32_t oob = rd(EXT_RGMII_OOB);
    oob &= ~(OOB_DISABLE | ID_MODE_DIS | RGMII_LINK);
    oob |= RGMII_MODE_EN;
    wr(EXT_RGMII_OOB, oob);

    dma_init();
    started = 1;
    linked = 0;
    last_link_check = 0;

    if (phy_start() != 0) { started = 0; return -1; }

    /* autonegotiation takes a few seconds; wait for it, but a missing cable is not an error */
    for (int i = 0; i < 40 && !linked; i++) {
        link_update();
        if (!linked) timer_delay_ms(100);
    }
    if (!linked) uart_puts("genet: no link yet (cable?), will keep looking\n");
    return 0;
}

static int genet_send(const uint8_t *frame, int len)
{
    if (!started || len <= 0 || len > ETH_MTU) return -1;
    link_update();
    if (!linked) return -1;

    uint32_t r = RING16(TDMA_DESC);
    int free_bds = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t cons = rd(r + R_IDX_A) & 0xFFFF;
        free_bds = NDESC - (int)((tx_producer - cons) & 0xFFFF);
        if (free_bds > 0) break;
        timer_delay_us(5);
    }
    if (free_bds <= 0) return -1;

    int i = (int)(tx_producer % NDESC);
    uint8_t *buf = tx_bufs + i * BUF_SIZE;
    memcpy(buf, frame, (size_t)len);
    int wire = len < 60 ? 60 : len;                       /* the controller does not pad */
    if (wire > len) memset(buf + len, 0, (size_t)(wire - len));
    __asm__ volatile("dsb sy" ::: "memory");

    uint64_t pa = (uint64_t)(uintptr_t)buf;
    uint32_t d = TDMA_DESC + (uint32_t)i * DESC_SIZE;
    wr(d + 4, (uint32_t)pa);
    wr(d + 8, (uint32_t)(pa >> 32));
    wr(d + 0, ((uint32_t)wire << 16) | D_QTAG | D_SOP | D_EOP | D_APPEND_CRC);

    tx_producer++;
    wr(r + R_IDX_B, tx_producer & 0xFFFF);
    return len;
}

static int genet_recv(uint8_t *frame, int max)
{
    if (!started) return 0;
    link_update();

    uint32_t r = RING16(RDMA_DESC);
    uint32_t producer = rd(r + R_IDX_A) & 0xFFFF;          /* the top half counts discards */
    if (producer == (rx_consumer & 0xFFFF)) return 0;

    int i = (int)(rx_consumer % NDESC);
    uint32_t status = rd(RDMA_DESC + (uint32_t)i * DESC_SIZE);
    int len = (int)((status >> 16) & 0xFFF);
    uint32_t flags = status & 0xFFFF;

    int ok = (flags & D_SOP) && (flags & D_EOP) && !(flags & D_RX_ERRORS) && len > 0 && len <= max;
    if (ok) memcpy(frame, rx_bufs + i * BUF_SIZE, (size_t)len);

    rx_consumer++;
    wr(r + R_IDX_B, rx_consumer & 0xFFFF);                 /* hand the descriptor back */
    return ok ? len : 0;
}

eth_driver_t genet_driver = { "GENET (Pi 4)", genet_start, genet_send, genet_recv, {0} };

/* The address is chosen during start, so it is copied out once the driver is picked. */
void genet_publish_mac(uint8_t *out) { memcpy(out, our_mac, 6); }
