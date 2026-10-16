#ifndef STRING_ARM_H
#define STRING_ARM_H

#include <stddef.h>

/* The few string routines the ARM port needs; the x86 build's string.c pulls in far more
 * than is wanted this early. */
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
void *memmove(void *d, const void *s, size_t n);
char *strchr(const char *s, int c);
int   strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
int   strcmp(const char *a, const char *b);
char *strstr(const char *h, const char *n);
char *strncpy(char *d, const char *s, size_t n);

#endif
