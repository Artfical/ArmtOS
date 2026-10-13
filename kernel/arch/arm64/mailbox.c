#include "mailbox.h"
#include "bcm.h"

/*
 * Property mailbox, channel 8. A request is a 16-byte-aligned buffer of tags; the firmware
 * writes its answers back into the same buffer, so it must live in memory the GPU can see
 * and must not be cached stale -- it is plain uncached memory here because the MMU is off.
 */

#define MBOX_BASE   (bcm_periph_base + 0xB880)
#define MBOX_READ   (MBOX_BASE + 0x00)
#define MBOX_STATUS (MBOX_BASE + 0x18)
#define MBOX_WRITE  (MBOX_BASE + 0x20)
#define MBOX_FULL   0x80000000
#define MBOX_EMPTY  0x40000000
#define MBOX_CH_PROP 8

/* The GPU sees ARM memory through a window; on the Pi 2/3 the uncached alias is at
 * 0xC0000000, which is what a mailbox buffer address has to be expressed in. */
#define GPU_BUS(addr) (((uint32_t)(uint64_t)(addr) & 0x3FFFFFFF) | 0xC0000000)

static volatile uint32_t mbox_buf[36] __attribute__((aligned(16)));

static int mailbox_call(void)
{
    uint32_t addr = GPU_BUS(mbox_buf);
    while (mmio_read(MBOX_STATUS) & MBOX_FULL) { }
    mmio_write(MBOX_WRITE, (addr & ~0xF) | MBOX_CH_PROP);
    for (;;) {
        while (mmio_read(MBOX_STATUS) & MBOX_EMPTY) { }
        uint32_t r = mmio_read(MBOX_READ);
        if ((r & 0xF) == MBOX_CH_PROP && (r & ~0xF) == (addr & ~0xF))
            return mbox_buf[1] == 0x80000000 ? 0 : -1;   /* the firmware sets this on success */
    }
}

int mailbox_board_revision(uint32_t *out)
{
    mbox_buf[0] = 7 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00010002;        /* get board revision */
    mbox_buf[3] = 4;
    mbox_buf[4] = 0;
    mbox_buf[5] = 0;
    mbox_buf[6] = 0;                 /* end tag */
    if (mailbox_call() != 0) return -1;
    *out = mbox_buf[5];
    return 0;
}

int mailbox_arm_memory(uint32_t *bytes)
{
    mbox_buf[0] = 8 * 4;
    mbox_buf[1] = 0;
    mbox_buf[2] = 0x00010005;        /* get ARM memory */
    mbox_buf[3] = 8;
    mbox_buf[4] = 0;
    mbox_buf[5] = 0;
    mbox_buf[6] = 0;
    mbox_buf[7] = 0;                 /* end tag */
    if (mailbox_call() != 0) return -1;
    *bytes = mbox_buf[6];
    return 0;
}

int mailbox_framebuffer(uint32_t width, uint32_t height,
                        uint64_t *fb_addr, uint32_t *pitch, uint32_t *fb_size)
{
    int i = 0;
    mbox_buf[i++] = 0;               /* size, filled in below */
    mbox_buf[i++] = 0;
    mbox_buf[i++] = 0x00048003; mbox_buf[i++] = 8; mbox_buf[i++] = 8; mbox_buf[i++] = width; mbox_buf[i++] = height;
    mbox_buf[i++] = 0x00048004; mbox_buf[i++] = 8; mbox_buf[i++] = 8; mbox_buf[i++] = width; mbox_buf[i++] = height;
    mbox_buf[i++] = 0x00048009; mbox_buf[i++] = 8; mbox_buf[i++] = 8; mbox_buf[i++] = 0;     mbox_buf[i++] = 0;
    mbox_buf[i++] = 0x00048005; mbox_buf[i++] = 4; mbox_buf[i++] = 4; mbox_buf[i++] = 32;    /* 32 bits per pixel */
    mbox_buf[i++] = 0x00048006; mbox_buf[i++] = 4; mbox_buf[i++] = 4; mbox_buf[i++] = 1;     /* RGB order */
    int fb_tag = i;
    mbox_buf[i++] = 0x00040001; mbox_buf[i++] = 8; mbox_buf[i++] = 8; mbox_buf[i++] = 4096;  mbox_buf[i++] = 0;
    int pitch_tag = i;
    mbox_buf[i++] = 0x00040008; mbox_buf[i++] = 4; mbox_buf[i++] = 4; mbox_buf[i++] = 0;
    mbox_buf[i++] = 0;               /* end tag */
    mbox_buf[0] = (uint32_t)(i * 4);

    if (mailbox_call() != 0) return -1;
    uint32_t addr = mbox_buf[fb_tag + 3];
    if (addr == 0) return -1;
    *fb_addr = addr & 0x3FFFFFFF;    /* back from a GPU bus address to an ARM one */
    *fb_size = mbox_buf[fb_tag + 4];
    *pitch = mbox_buf[pitch_tag + 3];
    return 0;
}
