#include "spi.h"
#include "bcm.h"
#include "gpio.h"
#include "irq.h"

#define SPI_BASE  (bcm_periph_base + 0x204000)
#define SPI_CS    (SPI_BASE + 0x00)
#define SPI_FIFO  (SPI_BASE + 0x04)
#define SPI_CLK   (SPI_BASE + 0x08)

#define CS_TA     (1u << 7)        /* transfer active */
#define CS_CLEAR  (3u << 4)
#define CS_DONE   (1u << 16)
#define CS_RXD    (1u << 17)
#define CS_TXD    (1u << 18)

int spi_init(uint32_t hz)
{
    if (!bcm_has_legacy_gpio) return -1;
    gpio_set_function(7, GPIO_ALT0);     /* CE1 */
    gpio_set_function(8, GPIO_ALT0);     /* CE0 */
    gpio_set_function(9, GPIO_ALT0);     /* MISO */
    gpio_set_function(10, GPIO_ALT0);    /* MOSI */
    gpio_set_function(11, GPIO_ALT0);    /* SCLK */

    /* The divider is a power of two of the 250 MHz core clock; anything else is rounded
     * down by the hardware, so it is rounded here instead to keep the rate predictable. */
    uint32_t div = 250000000u / (hz ? hz : 1000000);
    uint32_t p = 2;
    while (p < div && p < 32768) p <<= 1;
    mmio_write(SPI_CLK, p);
    mmio_write(SPI_CS, CS_CLEAR);
    return 0;
}

int spi_transfer(int chip_select, const uint8_t *tx, uint8_t *rx, int len)
{
    if (!bcm_has_legacy_gpio || len <= 0 || chip_select < 0 || chip_select > 2) return -1;
    mmio_write(SPI_CS, CS_CLEAR | (uint32_t)chip_select);
    mmio_write(SPI_CS, CS_CLEAR | (uint32_t)chip_select | CS_TA);

    uint64_t deadline = timer_us() + 50000;
    int sent = 0, got = 0;
    while (got < len && timer_us() < deadline) {
        while (sent < len && (mmio_read(SPI_CS) & CS_TXD))
            mmio_write(SPI_FIFO, tx ? tx[sent++] : (sent++, 0));
        while (got < len && (mmio_read(SPI_CS) & CS_RXD)) {
            uint8_t b = (uint8_t)mmio_read(SPI_FIFO);
            if (rx) rx[got] = b;
            got++;
        }
    }
    deadline = timer_us() + 10000;
    while (!(mmio_read(SPI_CS) & CS_DONE) && timer_us() < deadline) timer_delay_us(1);
    mmio_write(SPI_CS, CS_CLEAR | (uint32_t)chip_select);     /* drop the chip select */
    return got < len ? -1 : got;
}

int spi_adc_read(int chip_select, int channel)
{
    if (channel < 0 || channel > 7) return -1;
    /* The MCP3008 wants a start bit, then single-ended plus the channel, then idles while
     * it clocks the 10-bit answer back out across the next two bytes. */
    uint8_t tx[3] = { 1, (uint8_t)(0x80 | (channel << 4)), 0 };
    uint8_t rx[3] = { 0, 0, 0 };
    if (spi_transfer(chip_select, tx, rx, 3) != 3) return -1;
    return ((rx[1] & 0x03) << 8) | rx[2];
}
