#include "hwapi.h"
#include "gpio.h"
#include "pwm.h"
#include "i2c.h"
#include "spi.h"
#include "fb.h"
#include "bcm.h"
#include "irq.h"

/* Which PWM channel a pin belongs to, or -1 if it cannot do PWM at all. */
static int pwm_channel_for(int pin)
{
    if (pin == 12 || pin == 18) return 0;
    if (pin == 13 || pin == 19) return 1;
    return -1;
}

int hw_pin_mode(int pin, int mode)
{
    gpio_set_function(pin, mode == HW_PIN_OUT ? GPIO_OUT : GPIO_IN);
    return 0;
}

int hw_pin_pull(int pin, int pull)
{
    gpio_set_pull(pin, pull == HW_PULL_UP ? GPIO_PULL_UP :
                       pull == HW_PULL_DOWN ? GPIO_PULL_DOWN : GPIO_PULL_NONE);
    return 0;
}

int hw_pin_write(int pin, int value) { gpio_write(pin, value); return 0; }
int hw_pin_read(int pin)             { return gpio_read(pin); }

int hw_analog_write(int pin, int value)
{
    int ch = pwm_channel_for(pin);
    if (ch < 0) return -1;
    if (value < 0) value = 0;
    if (value > 1023) value = 1023;
    /* Starting the channel each time is cheap and means a caller never has to remember to
     * set it up first; the clock is only configured once inside the driver. */
    if (pwm_init(ch, pin, 1024) != 0) return -1;
    return pwm_write(ch, (uint32_t)value);
}

int hw_analog_read(int channel)
{
    if (spi_init(1000000) != 0) return -1;
    return spi_adc_read(0, channel);
}

int hw_i2c_setup(int hz) { return i2c_init((uint32_t)(hz > 0 ? hz : 100000)); }
int hw_i2c_write(int addr, const uint8_t *data, int len) { return i2c_write((uint8_t)addr, data, len); }
int hw_i2c_read(int addr, uint8_t *data, int len)        { return i2c_read((uint8_t)addr, data, len); }
int hw_i2c_read_reg(int addr, int reg, uint8_t *data, int len)
{
    return i2c_read_reg((uint8_t)addr, (uint8_t)reg, data, len);
}
int hw_i2c_scan(uint8_t *found, int max) { return i2c_scan(found, max); }

int hw_spi_setup(int hz) { return spi_init((uint32_t)(hz > 0 ? hz : 1000000)); }
int hw_spi_transfer(int cs, const uint8_t *tx, uint8_t *rx, int len)
{
    return spi_transfer(cs, tx, rx, len);
}

const char *hw_board_name(void)   { return bcm_model_name(); }
int         hw_board_generation(void) { return bcm_generation; }
uint64_t    hw_micros(void)       { return timer_us(); }
void        hw_delay_us(uint64_t us) { timer_delay_us(us); }

int hw_fb_setup(int width, int height)
{
    if (fb_ready()) return 0;
    return fb_init((uint32_t)width, (uint32_t)height);
}
void hw_fb_pixel(int x, int y, uint32_t rgb) { fb_pixel((uint32_t)x, (uint32_t)y, rgb); }
void hw_fb_rect(int x, int y, int w, int h, uint32_t rgb)
{
    if (w <= 0 || h <= 0) return;
    fb_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, rgb);
}
void hw_fb_text(int x, int y, const char *s, uint32_t rgb) { fb_text((uint32_t)x, (uint32_t)y, s, rgb); }
int  hw_fb_width(void)  { return (int)fb_width(); }
int  hw_fb_height(void) { return (int)fb_height(); }
