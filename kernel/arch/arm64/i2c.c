#include "i2c.h"
#include "bcm.h"
#include "gpio.h"
#include "irq.h"

/* The Broadcom serial controller. BSC1 is the one on the header pins. */

#define BSC_BASE   (bcm_periph_base + 0x804000)
#define BSC_C      (BSC_BASE + 0x00)
#define BSC_S      (BSC_BASE + 0x04)
#define BSC_DLEN   (BSC_BASE + 0x08)
#define BSC_A      (BSC_BASE + 0x0C)
#define BSC_FIFO   (BSC_BASE + 0x10)
#define BSC_DIV    (BSC_BASE + 0x14)

#define C_I2CEN    (1u << 15)
#define C_ST       (1u << 7)        /* start a transfer */
#define C_CLEAR    (1u << 4)
#define C_READ     (1u << 0)

#define S_CLKT     (1u << 9)        /* the device stretched the clock past the timeout */
#define S_ERR      (1u << 8)        /* no acknowledgement */
#define S_DONE     (1u << 1)
#define S_TXD      (1u << 4)        /* the FIFO can take a byte */
#define S_RXD      (1u << 5)        /* a byte is waiting */

int i2c_init(uint32_t hz)
{
    if (!bcm_has_legacy_gpio) return -1;
    gpio_set_function(2, GPIO_ALT0);
    gpio_set_function(3, GPIO_ALT0);
    /* The core clock is 150 MHz on the Pi 2/3 and 4; the divider is simply that over the
     * wanted bit rate. */
    uint32_t div = 150000000u / (hz ? hz : 100000);
    if (div > 0xFFFE) div = 0xFFFE;
    mmio_write(BSC_DIV, div & 0xFFFE);
    mmio_write(BSC_C, C_I2CEN);
    return 0;
}

/* Starts one transfer and waits for it, moving bytes as the FIFO allows. */
static int transfer(uint8_t addr, uint8_t *data, int len, int read)
{
    if (!bcm_has_legacy_gpio || len < 0 || len > 0xFFFF) return -1;
    mmio_write(BSC_A, addr & 0x7F);
    mmio_write(BSC_C, C_I2CEN | C_CLEAR);
    mmio_write(BSC_S, S_CLKT | S_ERR | S_DONE);     /* clear what the last transfer left */
    mmio_write(BSC_DLEN, (uint32_t)len);
    mmio_write(BSC_C, C_I2CEN | C_ST | (read ? C_READ : 0));

    /* A real transfer of this length takes well under a millisecond at 100 kHz, so the
     * deadline is generous; it exists so that a board with no such controller (or a bus
     * held low by a stuck device) reports a failure instead of spinning forever. */
    uint64_t deadline = timer_us() + 50000;
    int i = 0;
    while (timer_us() < deadline) {
        uint32_t s = mmio_read(BSC_S);
        if (s & (S_ERR | S_CLKT)) { mmio_write(BSC_S, S_ERR | S_CLKT); return -1; }
        if (read) {
            while ((mmio_read(BSC_S) & S_RXD) && i < len) data[i++] = (uint8_t)mmio_read(BSC_FIFO);
        } else {
            while ((mmio_read(BSC_S) & S_TXD) && i < len) mmio_write(BSC_FIFO, data[i++]);
        }
        if (s & S_DONE) {
            if (read) while ((mmio_read(BSC_S) & S_RXD) && i < len) data[i++] = (uint8_t)mmio_read(BSC_FIFO);
            mmio_write(BSC_S, S_DONE);
            return i;
        }
        timer_delay_us(2);
    }
    return -1;
}

int i2c_write(uint8_t addr, const uint8_t *data, int len) { return transfer(addr, (uint8_t *)data, len, 0); }
int i2c_read(uint8_t addr, uint8_t *data, int len)        { return transfer(addr, data, len, 1); }

int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, int len)
{
    if (i2c_write(addr, &reg, 1) < 0) return -1;
    return i2c_read(addr, data, len);
}

int i2c_scan(uint8_t *found, int max)
{
    int n = 0;
    uint8_t probe = 0;
    /* Nothing answers at all when the controller is absent; checking once up front keeps a
     * scan from taking its full timeout a hundred and twelve times over. */
    if (mmio_read(BSC_S) == 0 && mmio_read(BSC_DIV) == 0) return 0;
    /* A zero-length write is the usual way to ask "is anyone at this address?", but the
     * controller needs at least one byte, so a single read is used instead. */
    for (uint8_t a = 0x08; a < 0x78 && n < max; a++)
        if (transfer(a, &probe, 1, 1) >= 0) found[n++] = a;
    return n;
}
