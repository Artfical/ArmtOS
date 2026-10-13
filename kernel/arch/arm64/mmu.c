#include "mmu.h"
#include "bcm.h"
#include "uart.h"

/*
 * A flat, identity map built from 2 MB blocks.
 *
 * Two levels are enough: one level-1 table whose entries each cover 1 GB, pointing at
 * level-2 tables whose entries each cover 2 MB. RAM is mapped normal and cacheable, and
 * everything from the peripheral base upwards is mapped as device memory so that reads
 * and writes to registers are neither cached nor reordered.
 */

#define PAGE_SIZE   4096
#define BLOCK_2MB   (2ull * 1024 * 1024)
#define ENTRIES     512

/* descriptor bits */
#define DESC_TABLE  0x3
#define DESC_BLOCK  0x1
#define DESC_AF     (1ull << 10)          /* access flag: without it the first touch faults */
#define DESC_SH_INNER (3ull << 8)         /* inner shareable, so the caches stay coherent */
#define DESC_ATTR(i)  ((uint64_t)(i) << 2)

/* MAIR entries: 0 = device nGnRnE, 1 = normal write-back */
#define MAIR_VALUE  ((0x00ull << 0) | (0xFFull << 8))
#define ATTR_DEVICE 0
#define ATTR_NORMAL 1

/* How much address space to describe. Four gigabytes covers RAM and the peripherals on a
 * Pi 2 through 4; a Pi 5 puts its registers far higher, which is why the count is taken
 * from the peripheral base rather than fixed. */
#define L1_ENTRIES_MAX 512

static uint64_t l1[ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint64_t l2[8][ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static int enabled;

static int gigs_needed(void)
{
    uint64_t top = bcm_periph_base + 0x2000000;        /* past the peripheral block */
    int g = (int)((top + (1ull << 30) - 1) >> 30);
    if (g < 4) g = 4;
    if (g > 8) g = 8;                                  /* the tables above cover eight */
    return g;
}

void mmu_init(void)
{
    int gigs = gigs_needed();

    for (int i = 0; i < ENTRIES; i++) l1[i] = 0;
    for (int g = 0; g < gigs; g++) {
        for (int i = 0; i < ENTRIES; i++) {
            uint64_t addr = ((uint64_t)g << 30) | ((uint64_t)i * BLOCK_2MB);
            /* Anything at or above the peripheral base is a register window, not memory. */
            int device = addr >= bcm_periph_base;
            l2[g][i] = addr | DESC_BLOCK | DESC_AF
                     | (device ? DESC_ATTR(ATTR_DEVICE) : (DESC_ATTR(ATTR_NORMAL) | DESC_SH_INNER));
        }
        l1[g] = (uint64_t)(uintptr_t)l2[g] | DESC_TABLE;
    }

    __asm__ volatile("msr mair_el1, %0" :: "r"((uint64_t)MAIR_VALUE));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)l1));

    /* 39-bit addresses, 4 KB granule, inner/outer write-back on the table walks. */
    uint64_t tcr = (25ull << 0)      /* T0SZ: 2^39 bytes of address space */
                 | (0ull << 14)      /* TG0: 4 KB */
                 | (3ull << 12)      /* SH0: inner shareable */
                 | (1ull << 10)      /* ORGN0: write-back */
                 | (1ull << 8)       /* IRGN0: write-back */
                 | (1ull << 23);     /* EPD1: nothing is mapped through TTBR1 */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile("isb");

    /* Caches and TLBs must start clean, or stale entries from before the map existed are
     * used against the new one. */
    __asm__ volatile("tlbi vmalle1; dsb nsh; isb");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ull << 0);            /* M: the MMU itself */
    sctlr |= (1ull << 2);            /* C: data cache */
    sctlr |= (1ull << 12);           /* I: instruction cache */
    __asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr));

    enabled = 1;
}

int mmu_enabled(void) { return enabled; }
