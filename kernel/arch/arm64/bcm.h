#ifndef BCM_H
#define BCM_H

#include <stdint.h>

/*
 * The Broadcom SoC behind every Raspberry Pi. The peripherals sit at a base address that
 * moved with each generation, so it is resolved once at boot rather than compiled in:
 * one kernel8.img then runs on a Pi 2, 3 and 4 alike.
 */

extern uint64_t bcm_periph_base;
/* The console is not always in the peripheral block: on a Pi 5 the legacy peripherals
 * moved and the GPIO header is behind a separate chip, so the UART has its own base. */
extern uint64_t bcm_uart_base;
extern int      bcm_has_legacy_gpio;    /* 0 on a Pi 5, whose header is behind RP1 */
extern int      bcm_generation;         /* 2, 3, 4 or 5 */

void     bcm_detect(void);              /* works out which Pi this is */
const char *bcm_model_name(void);

static inline void mmio_write(uint64_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read(uint64_t addr)
{
    return *(volatile uint32_t *)addr;
}

/* a short pause for peripherals that need settling time between register writes */
static inline void bcm_delay(int32_t count)
{
    __asm__ volatile("1: subs %w0, %w0, #1\n bne 1b" : "+r"(count) :: "cc");
}

#define GPIO_BASE   (bcm_periph_base + 0x200000)
#define GPFSEL1     (GPIO_BASE + 0x04)
#define GPSET0      (GPIO_BASE + 0x1C)
#define GPCLR0      (GPIO_BASE + 0x28)
#define GPPUD       (GPIO_BASE + 0x94)
#define GPPUDCLK0   (GPIO_BASE + 0x98)
#define GPIO_PUP_PDN_CNTRL_REG0 (GPIO_BASE + 0xE4)   /* Pi 4 replaced the pull-up protocol */

#define AUX_BASE        (bcm_periph_base + 0x215000)
#define AUX_ENABLES     (AUX_BASE + 0x04)
#define AUX_MU_IO       (AUX_BASE + 0x40)
#define AUX_MU_IER      (AUX_BASE + 0x44)
#define AUX_MU_IIR      (AUX_BASE + 0x48)
#define AUX_MU_LCR      (AUX_BASE + 0x4C)
#define AUX_MU_MCR      (AUX_BASE + 0x50)
#define AUX_MU_LSR      (AUX_BASE + 0x54)
#define AUX_MU_CNTL     (AUX_BASE + 0x60)
#define AUX_MU_BAUD     (AUX_BASE + 0x68)

#define PL011_BASE  bcm_uart_base
#define PL011_DR    (PL011_BASE + 0x00)
#define PL011_FR    (PL011_BASE + 0x18)
#define PL011_IBRD  (PL011_BASE + 0x24)
#define PL011_FBRD  (PL011_BASE + 0x28)
#define PL011_LCRH  (PL011_BASE + 0x2C)
#define PL011_CR    (PL011_BASE + 0x30)
#define PL011_IMSC  (PL011_BASE + 0x38)
#define PL011_ICR   (PL011_BASE + 0x44)

#endif
