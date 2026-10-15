#include "irq.h"
#include "uart.h"
#include "heap.h"
#include "string_arm.h"
#include "compat/debugmon.h"
#include "compat/terminal.h"
#include "compat/klog.h"
#include "compat/serial.h"
#include "compat/scheduler.h"

/*
 * The little the x86 kernel's network stack asks of the rest of the kernel, answered for the
 * board: a millisecond clock, somewhere to print, a way to wait, and a few string and memory
 * routines. Everything else in kernel/net/ is compiled as it is.
 */

int net_verbose;

uint32_t debugmon_uptime_ms(void)       { return (uint32_t)(timer_us() / 1000); }
uint32_t debugmon_get_tick_count(void)  { return (uint32_t)(timer_us() / 10000); }   /* 100 Hz */

void terminal_putchar(char c)           { uart_putc(c); }
void terminal_writestring(const char *s) { uart_puts(s); }
void serial_write(const char *s)        { uart_puts(s); }

void klog_write(const char *s)          { if (net_verbose) uart_puts(s); }
void klog_write_hex(const char *label, const uint8_t *data, int len)
{
    if (!net_verbose) return;
    static const char hex[] = "0123456789abcdef";
    uart_puts(label);
    for (int i = 0; i < len; i++) { uart_putc(hex[data[i] >> 4]); uart_putc(hex[data[i] & 15]); }
    uart_putc('\n');
}

void task_yield(void)                   { __asm__ volatile("yield"); }
void task_sleep(uint32_t ms)            { timer_delay_ms(ms); }

void *krealloc(void *p, size_t n)       { return realloc(p, n); }
void *kcalloc(size_t a, size_t b)       { return calloc(a, b); }

/* string.h routines the ARM core does not provide (strcmp is in arm_mp.c) */
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strcat(char *d, const char *s)
{
    char *p = d + strlen(d);
    while ((*p++ = *s++)) { }
    return d;
}

char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return 0;
}

unsigned long atoul(const char *s)
{
    unsigned long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0');
    return v;
}

char *strtok(char *str, const char *delim)
{
    static char *save;
    if (str) save = str;
    if (!save) return 0;
    while (*save && strchr(delim, *save)) save++;
    if (!*save) { save = 0; return 0; }
    char *start = save;
    while (*save && !strchr(delim, *save)) save++;
    if (*save) *save++ = 0; else save = 0;
    return start;
}

/* ---- wall clock ---------------------------------------------------------------------
 * Certificate validity needs the date. Until something sets it, it is the moment this
 * kernel was built -- never earlier than the certificates it will meet, which is the
 * direction that matters for "not yet valid"; the SNTP client corrects it properly. */
#include "compat/cmos.h"

static uint64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)((int64_t)era * 146097 + (int64_t)doe - 719468);
}

static uint64_t build_epoch(void)
{
    static const char mons[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__, *t = __TIME__;          /* "Oct 14 2026", "18:32:07" */
    unsigned m = 1;
    for (unsigned i = 0; i < 12; i++) if (!strncmp(d, mons + i * 3, 3)) m = i + 1;
    unsigned day = (unsigned)((d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0'));
    int year = (d[7] - '0') * 1000 + (d[8] - '0') * 100 + (d[9] - '0') * 10 + (d[10] - '0');
    unsigned hh = (unsigned)((t[0] - '0') * 10 + (t[1] - '0'));
    unsigned mm = (unsigned)((t[3] - '0') * 10 + (t[4] - '0'));
    unsigned ss = (unsigned)((t[6] - '0') * 10 + (t[7] - '0'));
    return days_from_civil(year, m, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

static uint64_t clock_base;            /* epoch at clock_ref_us */
static uint64_t clock_ref_us;
static int clock_set;

uint64_t wallclock_epoch(void)
{
    if (!clock_base) { clock_base = build_epoch(); clock_ref_us = timer_us(); }
    return clock_base + (timer_us() - clock_ref_us) / 1000000;
}

void wallclock_set(uint64_t epoch)
{
    clock_base = epoch;
    clock_ref_us = timer_us();
    clock_set = 1;
}

int wallclock_is_set(void) { return clock_set; }

int cmos_get_time(cmos_time_t *t)
{
    uint64_t e = wallclock_epoch();
    uint64_t days = e / 86400, rem = e % 86400;
    t->hour = (int)(rem / 3600);
    t->minute = (int)(rem % 3600 / 60);
    t->second = (int)(rem % 60);
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    t->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    t->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    t->year = (int)(y + (t->month <= 2));
    return 0;
}
