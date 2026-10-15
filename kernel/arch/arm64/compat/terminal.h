#ifndef ARM_COMPAT_TERMINAL_H
#define ARM_COMPAT_TERMINAL_H
/* The x86 kernel prints to a VGA text terminal; here the same calls go to the serial console. */
void terminal_putchar(char c);
void terminal_writestring(const char *data);
#endif
