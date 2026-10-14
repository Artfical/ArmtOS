#ifndef ARM_DMA_H
#define ARM_DMA_H

#include <stddef.h>
#include <stdint.h>

/*
 * Memory a device can share with the CPU.
 *
 * RAM is mapped cacheable, so a buffer a device reads or writes would sit half in the CPU's
 * cache and half in memory, and each side would see the other's stale copy. Real hardware
 * does exactly that; QEMU, which has no such caches, hides it. This region is mapped
 * non-cacheable (mmu.c), so a buffer taken from it means the same thing to both sides with
 * no cache maintenance anywhere.
 */

#define DMA_REGION_BASE 0x4600000ull          /* just past the 64 MB heap (heap.c) */
#define DMA_REGION_SIZE 0x200000ull           /* one 2 MB block */

void *dma_alloc(size_t size);                 /* zeroed, 64-byte aligned, never freed; 0 if full */
uint32_t dma_gpu_bus(const volatile void *p);          /* the same address as the GPU-side DMA window sees it */

#endif
