#ifndef ARM_COMPAT_CMOS_H
#define ARM_COMPAT_CMOS_H
#include <stdint.h>
/* The x86 kernel reads the PC's battery-backed clock. A Pi has none, so the time is kept in
 * software: the build date at first, then whatever SNTP (or an HTTP Date header) says. */
typedef struct {
    int second;
    int minute;
    int hour;
    int day;
    int month;
    int year;
} cmos_time_t;
int cmos_get_time(cmos_time_t *time);

uint64_t wallclock_epoch(void);            /* seconds since 1970-01-01 UTC */
void     wallclock_set(uint64_t epoch);    /* call when a trustworthy time arrives */
int      wallclock_is_set(void);           /* 0 while it is still the build date */
#endif
