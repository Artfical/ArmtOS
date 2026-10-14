#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/misc.h"
#include "uart.h"
#include "irq.h"
#include <string.h>
#include "py/stream.h"

/*
 * What MicroPython needs from the board on ArmtOS: somewhere to print, somewhere to read
 * from, and a clock. All three are the serial console and the generic timer, which exist
 * long before anything else does.
 */

volatile int interrupt_char = -1;
void (*interrupt_callback)(void);

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) uart_putc(str[i]);
    return len;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len) { mp_hal_stdout_tx_strn(str, len); }
void mp_hal_stdout_tx_str(const char *str) { mp_hal_stdout_tx_strn(str, strlen(str)); }

/* Peeking at the port without taking the byte, which is what a poll has to do. */
int mp_hal_stdin_rx_chr_available(void)
{
    static int pending = -1;
    if (pending < 0) pending = uart_getc_nonblock();
    return pending >= 0;
}

int mp_hal_stdin_rx_chr(void)
{
    int c;
    while ((c = uart_getc_nonblock()) < 0) { }
    return c;
}

/* Real delays, from the counter -- a spin loop would be wrong by whatever the compiler and
 * the board's clock speed happened to make it. */
void mp_hal_delay_ms(mp_uint_t ms) { timer_delay_ms(ms); }
void mp_hal_delay_us(mp_uint_t us) { timer_delay_us(us); }
mp_uint_t mp_hal_ticks_ms(void)    { return (mp_uint_t)(timer_us() / 1000); }
mp_uint_t mp_hal_ticks_us(void)    { return (mp_uint_t)timer_us(); }
mp_uint_t mp_hal_ticks_cpu(void)   { return (mp_uint_t)timer_us(); }

/* Whether the console can be read or written without blocking. The serial port can always
 * take a byte, and has one to give only when its receive FIFO is not empty. */
uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags)
{
    uintptr_t ret = 0;
    if ((poll_flags & MP_STREAM_POLL_RD) && (mp_hal_stdin_rx_chr_available())) ret |= MP_STREAM_POLL_RD;
    if (poll_flags & MP_STREAM_POLL_WR) ret |= MP_STREAM_POLL_WR;
    return ret;
}
