#ifndef ARM_IRQ_H
#define ARM_IRQ_H

#include <stdint.h>

/*
 * Interrupts and time.
 *
 * The ARM generic timer is part of the core itself, so it works the same on every Pi --
 * unlike the SoC's own timer, whose address moved. The interrupt controller did not: the
 * Pi 2/3 have Broadcom's own, the Pi 4 a GIC-400, and the Pi 5 a GIC again, so the one in
 * use is chosen at boot.
 */

void arm_vectors_install(void);
void arm_irq_enable(void);
void arm_irq_disable(void);

void irq_init(void);
void timer_init(uint32_t hz);            /* starts the periodic tick */
uint64_t timer_ticks(void);              /* ticks since boot */
uint64_t timer_us(void);                 /* microseconds since boot, from the counter */
void timer_delay_us(uint64_t us);
void timer_delay_ms(uint64_t ms);

#endif
