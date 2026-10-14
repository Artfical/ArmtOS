#include "dma.h"
#include "string_arm.h"

static uint64_t next = DMA_REGION_BASE;

void *dma_alloc(size_t size)
{
    uint64_t start = (next + 63) & ~63ull;
    uint64_t end = start + ((size + 63) & ~(size_t)63);
    if (size == 0 || end > DMA_REGION_BASE + DMA_REGION_SIZE) return 0;
    next = end;
    memset((void *)start, 0, end - start);
    return (void *)start;
}

uint32_t dma_gpu_bus(const volatile void *p)
{
    return ((uint32_t)(uint64_t)p & 0x3FFFFFFF) | 0xC0000000;
}
