#ifndef ARM_PWM_H
#define ARM_PWM_H

#include <stdint.h>

/*
 * PWM, which is as close to an analog output as a Raspberry Pi has.
 *
 * There is no DAC on the board: a pin is either high or low. Switching it fast and varying
 * how long it stays high gives an average voltage, which is what drives an LED's brightness
 * or a motor's speed, and what a simple RC filter turns into a real analog level.
 */

/* Channel 0 is GPIO 12 or 18, channel 1 is GPIO 13 or 19. */
int  pwm_init(int channel, int gpio_pin, uint32_t range);
/* Sets the duty: `value` out of the range given to pwm_init(). */
int  pwm_write(int channel, uint32_t value);
/* Convenience: a duty in per cent. */
int  pwm_percent(int channel, uint32_t percent);
void pwm_stop(int channel);

#endif
