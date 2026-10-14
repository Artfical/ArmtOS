#ifndef HWAPI_H
#define HWAPI_H

#include <stdint.h>

/*
 * One place where every piece of hardware on the header is reachable by name.
 *
 * The shell, the `.t` syscall layer and the Python module all go through this rather than
 * poking registers themselves, so a board quirk is fixed once and every caller gets it.
 */

enum {
    HW_PIN_IN = 0, HW_PIN_OUT = 1,
    HW_PULL_NONE = 0, HW_PULL_DOWN = 1, HW_PULL_UP = 2
};

/* GPIO */
int hw_pin_mode(int pin, int mode);
int hw_pin_pull(int pin, int pull);
int hw_pin_write(int pin, int value);
int hw_pin_read(int pin);

/* "analog": a duty cycle out of 1024 on one of the four pins that can do PWM */
int hw_analog_write(int pin, int value_0_1023);
/* analog in through an MCP3008 on SPI; returns 0..1023 */
int hw_analog_read(int channel);

/* I2C */
int hw_i2c_setup(int hz);
int hw_i2c_write(int addr, const uint8_t *data, int len);
int hw_i2c_read(int addr, uint8_t *data, int len);
int hw_i2c_read_reg(int addr, int reg, uint8_t *data, int len);
int hw_i2c_scan(uint8_t *found, int max);

/* SPI */
int hw_spi_setup(int hz);
int hw_spi_transfer(int cs, const uint8_t *tx, uint8_t *rx, int len);

/* board and time */
const char *hw_board_name(void);
int      hw_board_generation(void);
uint64_t hw_micros(void);
void     hw_delay_us(uint64_t us);

/* display */
int  hw_fb_setup(int width, int height);
void hw_fb_pixel(int x, int y, uint32_t rgb);
void hw_fb_rect(int x, int y, int w, int h, uint32_t rgb);
void hw_fb_text(int x, int y, const char *s, uint32_t rgb);
int  hw_fb_width(void);
int  hw_fb_height(void);

#endif
