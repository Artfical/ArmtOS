#include "string_arm.h"

/* GCC emits calls to these even with -ffreestanding (for struct copies and array
 * initialisers), so they have to exist even where nothing calls them by name. */

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    while (n--) *a++ = *b++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* The rest of what the interpreter's C expects from a host's libc. */

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    if (a == b || n == 0) return d;
    /* Copy backwards when the ranges overlap the wrong way round, or the source is eaten
     * before it has been read. */
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return (char)c == 0 ? (char *)s : 0;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++)) { }
    return r;
}

/* The checked variants GCC emits with _FORTIFY_SOURCE; the bound is honoured rather than
 * ignored, so an overflow is caught here instead of corrupting whatever follows. */
void *__memcpy_chk(void *d, const void *s, size_t n, size_t dlen)
{
    if (n > dlen) for (;;) { }
    return memcpy(d, s, n);
}

void *__memmove_chk(void *d, const void *s, size_t n, size_t dlen)
{
    if (n > dlen) for (;;) { }
    return memmove(d, s, n);
}

void *__memset_chk(void *d, int c, size_t n, size_t dlen)
{
    if (n > dlen) for (;;) { }
    return memset(d, c, n);
}
