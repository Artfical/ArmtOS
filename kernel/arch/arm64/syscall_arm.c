#include "syscall_arm.h"
#include "hwapi.h"
#include "uart.h"
#include "irq.h"
#include "string_arm.h"
#include "loader.h"

void loader_return(void);

/*
 * The dispatcher. Every pointer a program passes is used only to read or write a small
 * bounded buffer, and the length is clamped here rather than trusted -- a program that
 * asked for a megabyte of I2C data would otherwise write a megabyte wherever it liked.
 */

#define MAX_BUF 256

/* A pointer from a program is only good if every byte of it lies inside the program's own
 * window; anything else would let it read or write the kernel through a system call. */
static int user_ok(uint64_t p, uint64_t n)
{
    if (p < USER_LOAD_ADDR || p >= USER_LOAD_ADDR + USER_REGION) return 0;
    return n <= USER_LOAD_ADDR + USER_REGION - p;
}
#define BAD ((uint64_t)-1)

uint64_t arm_syscall(uint64_t num, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    switch (num) {
    case SYS_WRITE: {
        const char *p = (const char *)b;
        uint64_t n = c > 4096 ? 4096 : c;
        if (!user_ok(b, n)) return BAD;
        for (uint64_t i = 0; i < n; i++) uart_putc(p[i]);
        return n;
    }
    case SYS_READ: {
        char *p = (char *)b;
        uint64_t n = c > 4096 ? 4096 : c;
        if (!user_ok(b, n)) return BAD;
        uint64_t i = 0;
        while (i < n) {
            char ch = uart_getc();
            p[i++] = ch;
            if (ch == '\n' || ch == '\r') break;
        }
        return i;
    }
    case SYS_GETPID:      return 1;
    case SYS_UPTIME_MS:   return hw_micros() / 1000;

    case SYS_PIN_MODE:    return (uint64_t)hw_pin_mode((int)a, (int)b);
    case SYS_PIN_PULL:    return (uint64_t)hw_pin_pull((int)a, (int)b);
    case SYS_PIN_WRITE:   return (uint64_t)hw_pin_write((int)a, (int)b);
    case SYS_PIN_READ:    return (uint64_t)hw_pin_read((int)a);
    case SYS_ANALOG_WRITE: return (uint64_t)hw_analog_write((int)a, (int)b);
    case SYS_ANALOG_READ:  return (uint64_t)hw_analog_read((int)a);

    case SYS_I2C_SETUP:   return (uint64_t)hw_i2c_setup((int)a);
    case SYS_I2C_WRITE:   if (!user_ok(b, c > MAX_BUF ? MAX_BUF : c)) return BAD; return (uint64_t)hw_i2c_write((int)a, (const uint8_t *)b, (int)(c > MAX_BUF ? MAX_BUF : c));
    case SYS_I2C_READ:    if (!user_ok(b, c > MAX_BUF ? MAX_BUF : c)) return BAD; return (uint64_t)hw_i2c_read((int)a, (uint8_t *)b, (int)(c > MAX_BUF ? MAX_BUF : c));
    case SYS_I2C_READ_REG: if (!user_ok(c, d > MAX_BUF ? MAX_BUF : d)) return BAD; return (uint64_t)hw_i2c_read_reg((int)a, (int)b, (uint8_t *)c, (int)(d > MAX_BUF ? MAX_BUF : d));
    case SYS_I2C_SCAN:    if (!user_ok(a, b > MAX_BUF ? MAX_BUF : b)) return BAD; return (uint64_t)hw_i2c_scan((uint8_t *)a, (int)(b > MAX_BUF ? MAX_BUF : b));

    case SYS_SPI_SETUP:   return (uint64_t)hw_spi_setup((int)a);
    case SYS_SPI_XFER:    { uint64_t n = d > MAX_BUF ? MAX_BUF : d; if (!user_ok(b, n) || !user_ok(c, n)) return BAD; } return (uint64_t)hw_spi_transfer((int)a, (const uint8_t *)b, (uint8_t *)c,
                                                           (int)(d > MAX_BUF ? MAX_BUF : d));

    case SYS_MICROS:      return hw_micros();
    case SYS_DELAY_US:    hw_delay_us(a); return 0;
    case SYS_BOARD_NAME: {
        const char *n = hw_board_name();
        char *out = (char *)a;
        uint64_t max = b > 64 ? 64 : b;
        if (!user_ok(a, max)) return BAD;
        uint64_t i = 0;
        while (i + 1 < max && n[i]) { out[i] = n[i]; i++; }
        if (max) out[i] = 0;
        return i;
    }

    case SYS_FB_SETUP:    return (uint64_t)hw_fb_setup((int)a, (int)b);
    case SYS_FB_PIXEL:    hw_fb_pixel((int)a, (int)b, (uint32_t)c); return 0;
    case SYS_FB_RECT:     hw_fb_rect((int)a, (int)b, (int)(c >> 16), (int)(c & 0xFFFF), (uint32_t)d); return 0;
    case SYS_FB_TEXT:     if (!user_ok(c, 1)) return BAD; { const char *s = (const char *)c; uint64_t lim = USER_LOAD_ADDR + USER_REGION - c, k = 0; while (k < lim && s[k]) k++; if (k == lim) return BAD; } hw_fb_text((int)a, (int)b, (const char *)c, (uint32_t)d); return 0;
    case SYS_FB_SIZE:     return ((uint64_t)hw_fb_width() << 16) | (uint64_t)hw_fb_height();

    case SYS_EXIT:
        /* There is nothing to return to in user mode, so control goes straight back to
         * whoever started the program. */
        loader_return();
        return 0;
    default:              return (uint64_t)-1;
    }
}
