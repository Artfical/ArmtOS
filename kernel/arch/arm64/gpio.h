#ifndef ARM_GPIO_H
#define ARM_GPIO_H

#include <stdint.h>

/*
 * The Pi's GPIO header: the reason most people pick this board. Pins are numbered as the
 * BCM numbering the header is documented with (GPIO 0-53), not by physical position.
 */

enum { GPIO_IN = 0, GPIO_OUT = 1, GPIO_ALT0 = 4, GPIO_ALT5 = 2 };
enum { GPIO_PULL_NONE = 0, GPIO_PULL_DOWN = 1, GPIO_PULL_UP = 2 };

void gpio_set_function(int pin, int function);
void gpio_set_pull(int pin, int pull);
void gpio_write(int pin, int high);
int  gpio_read(int pin);

#endif
