#ifndef ARM_COMPAT_KLOG_H
#define ARM_COMPAT_KLOG_H
#include <stdint.h>
/* The x86 kernel keeps a ring buffer for `dmesg`; here diagnostics go to the serial console
 * when net_verbose is set and are dropped otherwise. */
extern int net_verbose;
void klog_write(const char *s);
void klog_write_hex(const char *label, const uint8_t *data, int len);
#endif
