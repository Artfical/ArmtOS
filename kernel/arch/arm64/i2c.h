#ifndef ARM_I2C_H
#define ARM_I2C_H

#include <stdint.h>

/* I2C on GPIO 2 (data) and 3 (clock), the header's main sensor bus. */

int i2c_init(uint32_t hz);                                   /* 100000 or 400000 */
int i2c_write(uint8_t addr, const uint8_t *data, int len);   /* 7-bit address */
int i2c_read(uint8_t addr, uint8_t *data, int len);
/* Writes a register number then reads from it, the pattern nearly every chip uses. */
int i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, int len);
/* Reports which addresses answered, for finding out what is wired up. */
int i2c_scan(uint8_t *found, int max);

#endif
