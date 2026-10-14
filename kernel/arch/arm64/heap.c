#include "heap.h"
#include "string_arm.h"

/*
 * A first-fit free list over one region.
 *
 * Each block carries its size and whether it is free; freeing merges with the block after
 * it so that a long run of allocate/free does not leave the heap in fragments that nothing
 * fits into.
 */

extern char _end[];                        /* where the kernel image stops, from the linker */

#define HEAP_SIZE  (64 * 1024 * 1024)
#define HEAP_FLOOR 0x400000                /* first address past the user program window */
#define ALIGN(n)   (((n) + 15) & ~(size_t)15)
#define MAGIC      0x48454150u             /* "HEAP", to catch a pointer that is not ours */

typedef struct block {
    unsigned int  magic;
    unsigned int  free;
    size_t        size;                    /* payload bytes */
    struct block *next;
} block_t;

static block_t *head;
static char *heap_start, *heap_end;
static size_t used;

void heap_init(void)
{
    /* The window a .t program runs in (loader.h) is 0x200000-0x400000, and loading a program
     * overwrites it; a heap that started right after the kernel image would run through it. */
    size_t start = (size_t)(uintptr_t)_end;
    if (start < HEAP_FLOOR) start = HEAP_FLOOR;
    heap_start = (char *)ALIGN(start);
    heap_end = heap_start + HEAP_SIZE;
    head = (block_t *)heap_start;
    head->magic = MAGIC;
    head->free = 1;
    head->size = HEAP_SIZE - sizeof(block_t);
    head->next = 0;
    used = 0;
}

void *malloc(size_t size)
{
    if (!head) heap_init();
    if (size == 0) return 0;
    size = ALIGN(size);

    for (block_t *b = head; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        /* Split only when what is left can hold a block of its own, or the remainder would
         * be unreachable memory. */
        if (b->size >= size + sizeof(block_t) + 32) {
            block_t *n = (block_t *)((char *)(b + 1) + size);
            n->magic = MAGIC;
            n->free = 1;
            n->size = b->size - size - sizeof(block_t);
            n->next = b->next;
            b->next = n;
            b->size = size;
        }
        b->free = 0;
        used += b->size;
        return b + 1;
    }
    return 0;
}

void free(void *p)
{
    if (!p) return;
    block_t *b = (block_t *)p - 1;
    if (b->magic != MAGIC || b->free) return;     /* not ours, or already free */
    b->free = 1;
    used -= b->size;
    /* merge forward as far as the run of free blocks goes */
    while (b->next && b->next->free && b->next->magic == MAGIC) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
    }
}

void *realloc(void *p, size_t size)
{
    if (!p) return malloc(size);
    if (size == 0) { free(p); return 0; }
    block_t *b = (block_t *)p - 1;
    if (b->magic == MAGIC && b->size >= size) return p;
    void *n = malloc(size);
    if (!n) return 0;
    memcpy(n, p, b->magic == MAGIC && b->size < size ? b->size : size);
    free(p);
    return n;
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    if (n && total / n != size) return 0;         /* the multiplication overflowed */
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

size_t heap_used(void)  { return used; }
size_t heap_total(void) { return HEAP_SIZE; }
