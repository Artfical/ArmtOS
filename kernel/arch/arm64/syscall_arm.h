#ifndef SYSCALL_ARM_H
#define SYSCALL_ARM_H

#include <stdint.h>

/*
 * The system calls a `.t` program makes on ARM.
 *
 * A program issues `svc #0` with the call number in x8 and its arguments in x0-x3, which
 * is the AArch64 convention every ARM toolchain already follows -- the x86 build's
 * `int 0x80` has no equivalent here.
 *
 * The numbers below the hardware block match the x86 build's, so a program's source does
 * not have to change between the two; the hardware calls are new, because a PC has no
 * header to talk to.
 */

#define SYS_EXIT        1
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_GETPID      20
#define SYS_UPTIME_MS   207

/* the Raspberry Pi's own hardware */
#define SYS_PIN_MODE    300   /* a=pin, b=0 in / 1 out */
#define SYS_PIN_PULL    301   /* a=pin, b=0 none / 1 down / 2 up */
#define SYS_PIN_WRITE   302   /* a=pin, b=0|1 */
#define SYS_PIN_READ    303   /* a=pin -> 0|1 */
#define SYS_ANALOG_WRITE 304  /* a=pin, b=0..1023 (PWM) */
#define SYS_ANALOG_READ 305   /* a=channel -> 0..1023 (an MCP3008 on SPI) */
#define SYS_I2C_SETUP   310   /* a=hz */
#define SYS_I2C_WRITE   311   /* a=addr, b=buf, c=len */
#define SYS_I2C_READ    312   /* a=addr, b=buf, c=len */
#define SYS_I2C_READ_REG 313  /* a=addr, b=reg, c=buf, d=len */
#define SYS_I2C_SCAN    314   /* a=buf, b=max -> count */
#define SYS_SPI_SETUP   320   /* a=hz */
#define SYS_SPI_XFER    321   /* a=cs, b=tx, c=rx, d=len */
#define SYS_MICROS      330   /* -> microseconds since boot */
#define SYS_DELAY_US    331   /* a=microseconds */
#define SYS_BOARD_NAME  332   /* a=buf, b=len */
#define SYS_FB_SETUP    340   /* a=width, b=height */
#define SYS_FB_PIXEL    341   /* a=x, b=y, c=rgb */
#define SYS_FB_RECT     342   /* a=x, b=y, c=(w<<16|h), d=rgb */
#define SYS_FB_TEXT     343   /* a=x, b=y, c=string, d=rgb */
#define SYS_FB_SIZE     344   /* -> (width << 16) | height */

/* Called from the exception handler for an `svc` from EL0. */
uint64_t arm_syscall(uint64_t num, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

#endif
