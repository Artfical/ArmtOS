#ifndef ARM_UART_H
#define ARM_UART_H

#include <stdint.h>

/* The Pi's PL011 serial port: the console before anything else exists. */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);
int  uart_getc_nonblock(void);       /* -1 when nothing is waiting */
void uart_put_hex(uint64_t v);
void uart_put_dec(uint64_t v);

#endif
