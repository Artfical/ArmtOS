#ifndef ARM_SPI_H
#define ARM_SPI_H

#include <stdint.h>

/*
 * SPI on GPIO 7-11, which is how a Pi reaches an external ADC -- the board has no analog
 * input of its own, so a chip like an MCP3008 on this bus is what turns a voltage into a
 * number.
 */

int spi_init(uint32_t hz);
/* Exchanges bytes: SPI always sends and receives at once. Either buffer may be NULL. */
int spi_transfer(int chip_select, const uint8_t *tx, uint8_t *rx, int len);
/* Reads one channel of an MCP3008/3004, the common 10-bit ADC. Returns 0..1023, or -1. */
int spi_adc_read(int chip_select, int channel);

#endif
