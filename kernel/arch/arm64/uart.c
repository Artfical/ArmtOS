#include "bcm.h"
#include "uart.h"

/*
 * The PL011 UART on GPIO 14/15 -- the serial console every Pi exposes on its header, and
 * the only output that works before a display is set up, so it is what the early boot
 * reports through.
 */

void uart_init(void)
{
    mmio_write(PL011_CR, 0);                       /* off while it is reconfigured */

    /* GPIO 14 and 15 to their alternate function 0 (the UART). A Pi 5 has no GPIO block
     * here at all -- its header is behind RP1 -- and its debug UART needs no muxing. */
    if (!bcm_has_legacy_gpio) goto configure_uart;
    uint32_t sel = mmio_read(GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));
    sel |=  ((4u << 12) | (4u << 15));
    mmio_write(GPFSEL1, sel);

    /* Disable the pulls on those pins. The Pi 4 has a different register for this, so both
     * protocols are driven: the one that does not exist on a given chip is harmless. */
    mmio_write(GPPUD, 0);
    bcm_delay(150);
    mmio_write(GPPUDCLK0, (1u << 14) | (1u << 15));
    bcm_delay(150);
    mmio_write(GPPUDCLK0, 0);
    if (bcm_periph_base == 0xFE000000) {
        uint32_t p = mmio_read(GPIO_PUP_PDN_CNTRL_REG0);
        p &= ~((3u << 28) | (3u << 30));
        mmio_write(GPIO_PUP_PDN_CNTRL_REG0, p);
    }

configure_uart:
    mmio_write(PL011_ICR, 0x7FF);                  /* clear every pending interrupt */
    /* 115200 baud from the 48 MHz reference clock the firmware sets up:
     * divisor = 48e6 / (16 * 115200) = 26.0417 -> integer 26, fraction 3 */
    mmio_write(PL011_IBRD, 26);
    mmio_write(PL011_FBRD, 3);
    mmio_write(PL011_LCRH, (3 << 5) | (1 << 4));   /* 8 bits, FIFOs on */
    mmio_write(PL011_IMSC, 0);
    mmio_write(PL011_CR, (1 << 0) | (1 << 8) | (1 << 9));   /* enable, TX, RX */
}

void uart_putc(char c)
{
    if (c == '\n') uart_putc('\r');                /* a terminal expects both */
    while (mmio_read(PL011_FR) & (1 << 5)) { }     /* wait while the TX FIFO is full */
    mmio_write(PL011_DR, (uint32_t)(uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

int uart_getc_nonblock(void)
{
    if (mmio_read(PL011_FR) & (1 << 4)) return -1; /* RX FIFO empty */
    return (int)(mmio_read(PL011_DR) & 0xFF);
}

char uart_getc(void)
{
    int c;
    while ((c = uart_getc_nonblock()) < 0) { }
    return (char)c;
}

void uart_put_hex(uint64_t v)
{
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) uart_putc("0123456789ABCDEF"[(v >> i) & 0xF]);
}

void uart_put_dec(uint64_t v)
{
    char buf[24];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v && i < 23) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) uart_putc(buf[--i]);
}
