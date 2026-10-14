#ifndef ARM_HEAP_H
#define ARM_HEAP_H

#include <stddef.h>
#include <stdint.h>

/* A heap for the ARM build. Everything larger than a driver's own buffers -- a loaded
 * program, an interpreter's memory -- comes from here. */

void  heap_init(void);
void *malloc(size_t size);
void  free(void *p);
void *realloc(void *p, size_t size);
void *calloc(size_t n, size_t size);
size_t heap_used(void);
size_t heap_total(void);

#endif
