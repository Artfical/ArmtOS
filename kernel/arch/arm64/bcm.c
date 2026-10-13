#include "bcm.h"

/* Peripherals moved with each generation; 0x3F000000 is the Pi 2/3 address and the
 * default here, because that is also what QEMU's raspi3b models. */
uint64_t bcm_periph_base = 0x3F000000;

static const char *model = "Raspberry Pi 2/3 (BCM2836/BCM2837)";

void bcm_detect(void)
{
    /* The main ID register tells the core apart, which maps one-to-one onto the SoC:
     * Cortex-A7 = Pi 2, A53 = Pi 3, A72 = Pi 4. Reading a board register instead would
     * mean knowing the peripheral base already, which is what we are trying to find. */
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    uint32_t part = (uint32_t)((midr >> 4) & 0xFFF);
    switch (part) {
    case 0xD08:                                   /* Cortex-A72 */
        bcm_periph_base = 0xFE000000;
        model = "Raspberry Pi 4 (BCM2711)";
        break;
    case 0xD03:                                   /* Cortex-A53 */
        bcm_periph_base = 0x3F000000;
        model = "Raspberry Pi 3 (BCM2837)";
        break;
    case 0xC07:                                   /* Cortex-A7 */
        bcm_periph_base = 0x3F000000;
        model = "Raspberry Pi 2 (BCM2836)";
        break;
    default:
        bcm_periph_base = 0x3F000000;
        model = "Raspberry Pi (unrecognised core)";
        break;
    }
}

const char *bcm_model_name(void) { return model; }
