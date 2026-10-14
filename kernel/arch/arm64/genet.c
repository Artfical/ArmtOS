#include "net.h"
#include "bcm.h"
#include "uart.h"
#include "irq.h"
#include "string_arm.h"

/*
 * GENET, the Ethernet controller built into the Pi 4's SoC.
 *
 * It owns two rings of descriptors in main memory, one each way. A frame is sent by
 * writing its address and length into the next transmit descriptor and nudging the
 * producer index; received frames appear in the receive ring with the consumer index
 * trailing behind what the hardware has written.
 */

#define GENET_BASE      (bcm_periph_base + 0x1580000)

#define SYS_REV_CTRL    (GENET_BASE + 0x000)
#define SYS_RBUF_FLUSH  (GENET_BASE + 0x008)
#define UMAC_CMD        (GENET_BASE + 0x808)
#define UMAC_MAC0       (GENET_BASE + 0x80C)
#define UMAC_MAC1       (GENET_BASE + 0x810)
#define UMAC_MAX_FRAME  (GENET_BASE + 0x814)
#define RBUF_CTRL       (GENET_BASE + 0x300)
#define TBUF_CTRL       (GENET_BASE + 0x600)

#define CMD_TX_EN       (1u << 0)
#define CMD_RX_EN       (1u << 1)
#define CMD_SW_RESET    (1u << 13)

/* The descriptor rings; ring 16 is the default queue the hardware uses with no filtering. */
#define RDMA_BASE       (GENET_BASE + 0x2000)
#define TDMA_BASE       (GENET_BASE + 0x4000)
#define DESC_SIZE       12
#define DMA_RING(base)  ((base) + 0xC00)
#define RING_WRITE_PTR  0x08
#define RING_PROD_INDEX 0x08
#define RING_CONS_INDEX 0x0C
#define RING_BUF_SIZE   0x10
#define RING_START_ADDR 0x14
#define RING_END_ADDR   0x1C
#define RING_DMA_CTRL   0x04

#define NDESC 64
#define BUF_SIZE 2048

static uint8_t *rx_bufs, *tx_bufs;
static uint32_t rx_consumer;
static uint8_t our_mac[6];
static int started;

static uint64_t desc_addr(uint64_t base, int i) { return base + (uint64_t)i * DESC_SIZE; }

static int genet_start(void)
{
    /* A revision of all ones means there is no GENET here at all -- which is the normal
     * answer on a Pi 3, where the Ethernet is a USB device instead. */
    uint32_t rev = mmio_read(SYS_REV_CTRL);
    if (rev == 0xFFFFFFFF || rev == 0) return -1;

    mmio_write(UMAC_CMD, CMD_SW_RESET);
    timer_delay_us(10);
    mmio_write(UMAC_CMD, 0);

    /* The firmware programmed the address the board was given; it is read back rather than
     * invented, because a made-up address would not match what a DHCP lease was issued to. */
    uint32_t m0 = mmio_read(UMAC_MAC0), m1 = mmio_read(UMAC_MAC1);
    our_mac[0] = (uint8_t)(m0 >> 24); our_mac[1] = (uint8_t)(m0 >> 16);
    our_mac[2] = (uint8_t)(m0 >> 8);  our_mac[3] = (uint8_t)m0;
    our_mac[4] = (uint8_t)(m1 >> 8);  our_mac[5] = (uint8_t)m1;

    mmio_write(UMAC_MAX_FRAME, ETH_MTU);
    mmio_write(RBUF_CTRL, mmio_read(RBUF_CTRL) | (1u << 1));    /* align the IP header */

    extern void *malloc(unsigned long);
    rx_bufs = (uint8_t *)malloc(NDESC * BUF_SIZE);
    tx_bufs = (uint8_t *)malloc(NDESC * BUF_SIZE);
    if (!rx_bufs || !tx_bufs) return -1;

    for (int i = 0; i < NDESC; i++) {
        uint64_t d = desc_addr(RDMA_BASE, i);
        uint64_t buf = (uint64_t)(uintptr_t)(rx_bufs + i * BUF_SIZE);
        mmio_write(d + 4, (uint32_t)buf);
        mmio_write(d + 8, (uint32_t)(buf >> 32));
        mmio_write(d + 0, BUF_SIZE << 16);
    }

    mmio_write(DMA_RING(RDMA_BASE) + RING_START_ADDR, 0);
    mmio_write(DMA_RING(RDMA_BASE) + RING_END_ADDR, NDESC * DESC_SIZE / 4 - 1);
    mmio_write(DMA_RING(TDMA_BASE) + RING_START_ADDR, 0);
    mmio_write(DMA_RING(TDMA_BASE) + RING_END_ADDR, NDESC * DESC_SIZE / 4 - 1);

    rx_consumer = 0;
    mmio_write(UMAC_CMD, CMD_TX_EN | CMD_RX_EN);
    started = 1;
    return 0;
}

static int genet_send(const uint8_t *frame, int len)
{
    if (!started || len <= 0 || len > ETH_MTU) return -1;
    static uint32_t producer;
    int i = (int)(producer % NDESC);
    memcpy(tx_bufs + i * BUF_SIZE, frame, (size_t)len);
    uint64_t buf = (uint64_t)(uintptr_t)(tx_bufs + i * BUF_SIZE);
    uint64_t d = desc_addr(TDMA_BASE, i);
    mmio_write(d + 4, (uint32_t)buf);
    mmio_write(d + 8, (uint32_t)(buf >> 32));
    /* length, plus the bits saying this descriptor is a whole frame and wants a checksum */
    mmio_write(d + 0, ((uint32_t)len << 16) | (1u << 15) | (1u << 14) | (1u << 13));
    producer++;
    mmio_write(DMA_RING(TDMA_BASE) + RING_PROD_INDEX, producer & 0xFFFF);
    return len;
}

static int genet_recv(uint8_t *frame, int max)
{
    if (!started) return 0;
    uint32_t producer = mmio_read(DMA_RING(RDMA_BASE) + RING_PROD_INDEX) & 0xFFFF;
    if (producer == rx_consumer) return 0;

    int i = (int)(rx_consumer % NDESC);
    uint32_t status = mmio_read(desc_addr(RDMA_BASE, i));
    int len = (int)((status >> 16) & 0xFFF);
    if (len > max) len = max;
    if (len > 0) memcpy(frame, rx_bufs + i * BUF_SIZE, (size_t)len);

    rx_consumer++;
    mmio_write(DMA_RING(RDMA_BASE) + RING_CONS_INDEX, rx_consumer & 0xFFFF);
    return len;
}

eth_driver_t genet_driver = { "GENET (Pi 4)", genet_start, genet_send, genet_recv, {0} };

/* The address is read during start, so it is copied out once the driver is chosen. */
void genet_publish_mac(uint8_t *out) { memcpy(out, our_mac, 6); }
