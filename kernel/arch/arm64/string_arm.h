#ifndef STRING_ARM_H
#define STRING_ARM_H

#include <stddef.h>

/* The few string routines the ARM port needs; the x86 build's string.c pulls in far more
 * than is wanted this early. */
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
