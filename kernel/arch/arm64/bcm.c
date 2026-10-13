#include "bcm.h"

/*
 * Which Raspberry Pi this is.
 *
 * The core tells us: each generation used a different one, and the SoC follows from it.
 * That matters because almost every address below moved at least once -- a kernel that
 * assumed the Pi 3's layout writes into nothing on a Pi 4, and into the wrong thing on a
 * Pi 5.
 */

uint64_t bcm_periph_base = 0x3F000000;
uint64_t bcm_uart_base   = 0x3F201000;
int      bcm_has_legacy_gpio = 1;
int      bcm_generation = 3;

static const char *model = "Raspberry Pi 2/3 (BCM2836/BCM2837)";

void bcm_detect(void)
{
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    uint32_t part = (uint32_t)((midr >> 4) & 0xFFF);

    switch (part) {
    case 0xD0B:                                   /* Cortex-A76: Pi 5 */
        /* The BCM2712 moved the legacy peripherals far up the address map, and the GPIO
         * header, USB and Ethernet all live in the separate RP1 chip across PCIe -- so the
         * old GPIO block simply is not there, and pretending otherwise would write into
         * whatever happens to be mapped at that address. */
        bcm_periph_base = 0x107C000000ull;
        bcm_uart_base   = 0x107D001000ull;        /* the debug UART on the 3-pin connector */
        bcm_has_legacy_gpio = 0;
        bcm_generation = 5;
        model = "Raspberry Pi 5 (BCM2712)";
        break;
    case 0xD08:                                   /* Cortex-A72: Pi 4 */
        bcm_periph_base = 0xFE000000;
        bcm_uart_base   = 0xFE201000;
        bcm_has_legacy_gpio = 1;
        bcm_generation = 4;
        model = "Raspberry Pi 4 (BCM2711)";
        break;
    case 0xD03:                                   /* Cortex-A53: Pi 3 */
        bcm_periph_base = 0x3F000000;
        bcm_uart_base   = 0x3F201000;
        bcm_generation = 3;
        model = "Raspberry Pi 3 (BCM2837)";
        break;
    case 0xC07:                                   /* Cortex-A7: Pi 2 */
        bcm_periph_base = 0x3F000000;
        bcm_uart_base   = 0x3F201000;
        bcm_generation = 2;
        model = "Raspberry Pi 2 (BCM2836)";
        break;
    default:
        bcm_periph_base = 0x3F000000;
        bcm_uart_base   = 0x3F201000;
        bcm_generation = 3;
        model = "Raspberry Pi (unrecognised core)";
        break;
    }
}

const char *bcm_model_name(void) { return model; }
