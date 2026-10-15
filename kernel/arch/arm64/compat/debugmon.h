#ifndef ARM_COMPAT_DEBUGMON_H
#define ARM_COMPAT_DEBUGMON_H
#include <stdint.h>
/* Stands in for the x86 kernel's debug monitor: the network stack only wants a clock. */
uint32_t debugmon_uptime_ms(void);
uint32_t debugmon_get_tick_count(void);
#endif
