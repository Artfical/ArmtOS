#include "gpio.h"
#include "bcm.h"

/* Each register covers a block of pins, so a pin number picks both the register and the
 * field inside it: 10 pins per function register (3 bits each), 32 per level register. */

void gpio_set_function(int pin, int function)
{
    if (pin < 0 || pin > 53 || !bcm_has_legacy_gpio) return;
    uint64_t reg = GPIO_BASE + (uint64_t)(pin / 10) * 4;
    int shift = (pin % 10) * 3;
    uint32_t v = mmio_read(reg);
    v &= ~(7u << shift);
    v |= ((uint32_t)function & 7u) << shift;
    mmio_write(reg, v);
}

void gpio_set_pull(int pin, int pull)
{
    if (pin < 0 || pin > 53 || !bcm_has_legacy_gpio) return;

    if (bcm_generation == 4) {
        /* The Pi 4 replaced the old clocked sequence with a plain register: 0 none,
         * 1 pull-up, 2 pull-down -- note the order is not the same as the old one. */
        uint64_t reg = GPIO_PUP_PDN_CNTRL_REG0 + (uint64_t)(pin / 16) * 4;
        int shift = (pin % 16) * 2;
        uint32_t want = pull == GPIO_PULL_UP ? 1 : pull == GPIO_PULL_DOWN ? 2 : 0;
        uint32_t v = mmio_read(reg);
        v &= ~(3u << shift);
        v |= want << shift;
        mmio_write(reg, v);
        return;
    }

    /* Older chips latch the setting: write the mode, pulse the clock for the pin, clear. */
    mmio_write(GPPUD, (uint32_t)pull);
    bcm_delay(150);
    mmio_write(GPPUDCLK0 + (uint64_t)(pin / 32) * 4, 1u << (pin % 32));
    bcm_delay(150);
    mmio_write(GPPUD, 0);
    mmio_write(GPPUDCLK0 + (uint64_t)(pin / 32) * 4, 0);
}

void gpio_write(int pin, int high)
{
    if (pin < 0 || pin > 53 || !bcm_has_legacy_gpio) return;
    /* Separate set and clear registers, so changing one pin never disturbs another. */
    uint64_t reg = (high ? GPSET0 : GPCLR0) + (uint64_t)(pin / 32) * 4;
    mmio_write(reg, 1u << (pin % 32));
}

int gpio_read(int pin)
{
    if (pin < 0 || pin > 53 || !bcm_has_legacy_gpio) return -1;
    uint64_t lev = GPIO_BASE + 0x34 + (uint64_t)(pin / 32) * 4;
    return (mmio_read(lev) >> (pin % 32)) & 1;
}
