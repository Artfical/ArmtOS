#include "pwm.h"
#include "bcm.h"
#include "gpio.h"
#include "irq.h"

/*
 * The PWM block runs from its own clock, which has to be stopped before its divider can be
 * changed -- writing the divider while it runs leaves the block in an undefined state that
 * only a power cycle clears.
 */

#define PWM_BASE   (bcm_periph_base + 0x20C000)
#define PWM_CTL    (PWM_BASE + 0x00)
#define PWM_STA    (PWM_BASE + 0x04)
#define PWM_RNG1   (PWM_BASE + 0x10)
#define PWM_DAT1   (PWM_BASE + 0x14)
#define PWM_RNG2   (PWM_BASE + 0x20)
#define PWM_DAT2   (PWM_BASE + 0x24)

#define CM_BASE    (bcm_periph_base + 0x101000)
#define CM_PWMCTL  (CM_BASE + 0xA0)
#define CM_PWMDIV  (CM_BASE + 0xA4)
#define CM_PASSWD  0x5A000000

#define CTL_PWEN1  (1u << 0)
#define CTL_MSEN1  (1u << 7)       /* mark/space: a plain duty cycle rather than the balanced mode */
#define CTL_PWEN2  (1u << 8)
#define CTL_MSEN2  (1u << 15)

static uint32_t chan_range[2] = { 1024, 1024 };
static int clock_ready;

/* The oscillator is 19.2 MHz on a Pi up to the 4; dividing it by 2 gives a PWM carrier well
 * above anything a motor or an eye notices for the ranges used here. */
static void pwm_clock_init(void)
{
    if (clock_ready) return;
    mmio_write(CM_PWMCTL, CM_PASSWD | (1 << 5));           /* stop the clock */
    for (int i = 0; i < 10000 && (mmio_read(CM_PWMCTL) & (1 << 7)); i++) timer_delay_us(10);
    mmio_write(CM_PWMDIV, CM_PASSWD | (2 << 12));          /* integer divider of 2 */
    mmio_write(CM_PWMCTL, CM_PASSWD | 1);                  /* source: the 19.2 MHz oscillator */
    mmio_write(CM_PWMCTL, CM_PASSWD | 1 | (1 << 4));       /* and enable */
    for (int i = 0; i < 10000 && !(mmio_read(CM_PWMCTL) & (1 << 7)); i++) timer_delay_us(10);
    clock_ready = 1;
}

int pwm_init(int channel, int gpio_pin, uint32_t range)
{
    if (channel < 0 || channel > 1 || range == 0 || !bcm_has_legacy_gpio) return -1;

    /* Only four pins can carry PWM, and which alternate function selects it differs
     * between them -- 12 and 13 use ALT0, 18 and 19 use ALT5. */
    int alt;
    if (gpio_pin == 12 || gpio_pin == 13) alt = GPIO_ALT0;
    else if (gpio_pin == 18 || gpio_pin == 19) alt = GPIO_ALT5;
    else return -1;
    gpio_set_function(gpio_pin, alt);

    pwm_clock_init();
    chan_range[channel] = range;

    uint32_t ctl = mmio_read(PWM_CTL);
    if (channel == 0) {
        mmio_write(PWM_RNG1, range);
        mmio_write(PWM_DAT1, 0);
        ctl |= CTL_PWEN1 | CTL_MSEN1;
    } else {
        mmio_write(PWM_RNG2, range);
        mmio_write(PWM_DAT2, 0);
        ctl |= CTL_PWEN2 | CTL_MSEN2;
    }
    mmio_write(PWM_CTL, ctl);
    return 0;
}

int pwm_write(int channel, uint32_t value)
{
    if (channel < 0 || channel > 1 || !bcm_has_legacy_gpio) return -1;
    if (value > chan_range[channel]) value = chan_range[channel];
    mmio_write(channel == 0 ? PWM_DAT1 : PWM_DAT2, value);
    return 0;
}

int pwm_percent(int channel, uint32_t percent)
{
    if (channel < 0 || channel > 1) return -1;
    if (percent > 100) percent = 100;
    return pwm_write(channel, chan_range[channel] * percent / 100);
}

void pwm_stop(int channel)
{
    if (channel < 0 || channel > 1 || !bcm_has_legacy_gpio) return;
    uint32_t ctl = mmio_read(PWM_CTL);
    ctl &= ~(channel == 0 ? (CTL_PWEN1 | CTL_MSEN1) : (CTL_PWEN2 | CTL_MSEN2));
    mmio_write(PWM_CTL, ctl);
}
