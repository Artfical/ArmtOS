#ifndef ARMTOS_H
#define ARMTOS_H

/*
 * The ArmtOS program interface.
 *
 * A `.t` program is a flat AArch64 binary. It reaches the kernel with `svc #0`, the call
 * number in x8 and arguments in x0-x3 -- the ordinary AArch64 convention, so a program can
 * be written in plain C with no assembly beyond what is here.
 *
 *   armtos-cc blink.c -o blink.t
 */

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* system call numbers; these match kernel/arch/arm64/syscall_arm.h */
#define SYS_EXIT         1
#define SYS_READ         3
#define SYS_WRITE        4
#define SYS_GETPID       20
#define SYS_UPTIME_MS    207
#define SYS_PIN_MODE     300
#define SYS_PIN_PULL     301
#define SYS_PIN_WRITE    302
#define SYS_PIN_READ     303
#define SYS_ANALOG_WRITE 304
#define SYS_ANALOG_READ  305
#define SYS_I2C_SETUP    310
#define SYS_I2C_WRITE    311
#define SYS_I2C_READ     312
#define SYS_I2C_READ_REG 313
#define SYS_I2C_SCAN     314
#define SYS_SPI_SETUP    320
#define SYS_SPI_XFER     321
#define SYS_MICROS       330
#define SYS_DELAY_US     331
#define SYS_BOARD_NAME   332
#define SYS_FB_SETUP     340
#define SYS_FB_PIXEL     341
#define SYS_FB_RECT      342
#define SYS_FB_TEXT      343
#define SYS_FB_SIZE      344

static inline long sys(long n, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

/* ---- console ---- */
static inline int   str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline void  print(const char *s)   { sys(SYS_WRITE, 1, (long)s, str_len(s), 0); }
static inline void  print_num(long v)
{
    char b[24];
    int i = 23;
    int neg = v < 0;
    unsigned long x = neg ? (unsigned long)(-v) : (unsigned long)v;
    b[i] = 0;
    do { b[--i] = (char)('0' + x % 10); x /= 10; } while (x);
    if (neg) b[--i] = '-';
    print(b + i);
}
static inline int   read_line(char *buf, int max) { return (int)sys(SYS_READ, 0, (long)buf, max, 0); }
static inline void  exit_program(int code) { sys(SYS_EXIT, code, 0, 0, 0); for (;;) { } }

/* ---- pins ---- */
#define INPUT  0
#define OUTPUT 1
#define PULL_NONE 0
#define PULL_DOWN 1
#define PULL_UP   2
#define LOW  0
#define HIGH 1

static inline void pin_mode(int pin, int mode)  { sys(SYS_PIN_MODE, pin, mode, 0, 0); }
static inline void pin_pull(int pin, int pull)  { sys(SYS_PIN_PULL, pin, pull, 0, 0); }
static inline void pin_write(int pin, int v)    { sys(SYS_PIN_WRITE, pin, v, 0, 0); }
static inline int  pin_read(int pin)            { return (int)sys(SYS_PIN_READ, pin, 0, 0, 0); }

/* A duty cycle out of 1023 on GPIO 12, 13, 18 or 19 -- the board has no real DAC. */
static inline int  analog_write(int pin, int value) { return (int)sys(SYS_ANALOG_WRITE, pin, value, 0, 0); }
/* A channel of an MCP3008 wired to SPI; the Pi itself cannot measure a voltage. */
static inline int  analog_read(int channel)        { return (int)sys(SYS_ANALOG_READ, channel, 0, 0, 0); }

/* ---- time ---- */
static inline u64  micros(void)            { return (u64)sys(SYS_MICROS, 0, 0, 0, 0); }
static inline u64  millis(void)            { return (u64)sys(SYS_UPTIME_MS, 0, 0, 0, 0); }
static inline void delay_us(u64 us)        { sys(SYS_DELAY_US, (long)us, 0, 0, 0); }
static inline void delay_ms(u64 ms)        { sys(SYS_DELAY_US, (long)(ms * 1000), 0, 0, 0); }

/* ---- I2C ---- */
static inline int i2c_setup(int hz)                                 { return (int)sys(SYS_I2C_SETUP, hz, 0, 0, 0); }
static inline int i2c_write(int addr, const u8 *d, int n)           { return (int)sys(SYS_I2C_WRITE, addr, (long)d, n, 0); }
static inline int i2c_read(int addr, u8 *d, int n)                  { return (int)sys(SYS_I2C_READ, addr, (long)d, n, 0); }
static inline int i2c_read_reg(int addr, int reg, u8 *d, int n)     { return (int)sys(SYS_I2C_READ_REG, addr, reg, (long)d, n); }
static inline int i2c_scan(u8 *found, int max)                      { return (int)sys(SYS_I2C_SCAN, (long)found, max, 0, 0); }

/* ---- SPI ---- */
static inline int spi_setup(int hz)                                 { return (int)sys(SYS_SPI_SETUP, hz, 0, 0, 0); }
static inline int spi_transfer(int cs, const u8 *tx, u8 *rx, int n) { return (int)sys(SYS_SPI_XFER, cs, (long)tx, (long)rx, n); }

/* ---- board ---- */
static inline int board_name(char *buf, int max) { return (int)sys(SYS_BOARD_NAME, (long)buf, max, 0, 0); }

/* ---- display ---- */
static inline int  fb_setup(int w, int h)                  { return (int)sys(SYS_FB_SETUP, w, h, 0, 0); }
static inline void fb_pixel(int x, int y, u32 rgb)         { sys(SYS_FB_PIXEL, x, y, (long)rgb, 0); }
static inline void fb_rect(int x, int y, int w, int h, u32 rgb) { sys(SYS_FB_RECT, x, y, (w << 16) | (h & 0xFFFF), (long)rgb); }
static inline void fb_text(int x, int y, const char *s, u32 rgb) { sys(SYS_FB_TEXT, x, y, (long)s, (long)rgb); }
static inline int  fb_width(void)  { return (int)(sys(SYS_FB_SIZE, 0, 0, 0, 0) >> 16); }
static inline int  fb_height(void) { return (int)(sys(SYS_FB_SIZE, 0, 0, 0, 0) & 0xFFFF); }

/* Every program starts here; the runtime below calls it and exits cleanly afterwards. */
void main_(void);

#endif
