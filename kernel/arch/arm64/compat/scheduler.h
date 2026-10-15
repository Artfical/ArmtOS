#ifndef ARM_COMPAT_SCHEDULER_H
#define ARM_COMPAT_SCHEDULER_H
#include <stdint.h>
/* There is no scheduler on the board yet: waiting means spinning, yielding means a hint. */
void task_yield(void);
void task_sleep(uint32_t ms);
#endif
