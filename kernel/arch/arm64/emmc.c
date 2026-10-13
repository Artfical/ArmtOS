#include "emmc.h"
#include "bcm.h"
#include "uart.h"
#include "irq.h"

/*
 * Broadcom's SD host controller, which follows the standard SDHCI register layout.
 *
 * Bringing a card up is a fixed conversation: reset the host, clock it slowly, ask the
 * card what it is (CMD0/CMD8/ACMD41), learn its address (CMD2/CMD3), select it (CMD7),
 * widen the bus and speed the clock up. Only then can blocks be read.
 */

#define EMMC_BASE      (bcm_periph_base + 0x300000)
#define EMMC_ARG2      (EMMC_BASE + 0x00)
#define EMMC_BLKSIZECNT (EMMC_BASE + 0x04)
#define EMMC_ARG1      (EMMC_BASE + 0x08)
#define EMMC_CMDTM     (EMMC_BASE + 0x0C)
#define EMMC_RESP0     (EMMC_BASE + 0x10)
#define EMMC_RESP1     (EMMC_BASE + 0x14)
#define EMMC_RESP2     (EMMC_BASE + 0x18)
#define EMMC_RESP3     (EMMC_BASE + 0x1C)
#define EMMC_DATA      (EMMC_BASE + 0x20)
#define EMMC_STATUS    (EMMC_BASE + 0x24)
#define EMMC_CONTROL0  (EMMC_BASE + 0x28)
#define EMMC_CONTROL1  (EMMC_BASE + 0x2C)
#define EMMC_INTERRUPT (EMMC_BASE + 0x30)
#define EMMC_IRPT_MASK (EMMC_BASE + 0x34)
#define EMMC_IRPT_EN   (EMMC_BASE + 0x38)
#define EMMC_SLOTISR_VER (EMMC_BASE + 0xFC)

/* CONTROL1 */
#define C1_CLK_INTLEN  (1u << 0)
#define C1_CLK_STABLE  (1u << 1)
#define C1_CLK_EN      (1u << 2)
#define C1_SRST_HC     (1u << 24)
#define C1_SRST_CMD    (1u << 25)
#define C1_SRST_DATA   (1u << 26)

/* INTERRUPT */
#define INT_CMD_DONE   (1u << 0)
#define INT_DATA_DONE  (1u << 1)
#define INT_READ_RDY   (1u << 5)
#define INT_WRITE_RDY  (1u << 4)
#define INT_ERR        (1u << 15)

/* STATUS */
#define SR_CMD_INHIBIT  (1u << 0)
#define SR_DAT_INHIBIT  (1u << 1)

/* command flags in CMDTM */
#define CMD_RSPNS_136  (1u << 16)
#define CMD_RSPNS_48   (2u << 16)
#define CMD_RSPNS_48B  (3u << 16)
#define CMD_CRCCHK_EN  (1u << 19)
#define CMD_IS_DATA    (1u << 21)
#define CMD_DAT_READ   (1u << 4)
#define CMD_DAT_MULTI  (1u << 5)
#define CMD_BLKCNT_EN  (1u << 1)
#define CMD_INDEX(n)   ((uint32_t)(n) << 24)

static uint32_t card_rca;
static int card_is_sdhc;            /* an SDHC/SDXC card is addressed in blocks, not bytes */
static uint64_t card_blocks;

static int wait_status(uint32_t mask, int ms)
{
    for (int i = 0; i < ms * 100; i++) {
        if (!(mmio_read(EMMC_STATUS) & mask)) return 0;
        timer_delay_us(10);
    }
    return -1;
}

/* Waits for one of the interrupt bits; returns -1 on an error bit or a timeout. */
static int wait_int(uint32_t mask, int ms)
{
    for (int i = 0; i < ms * 100; i++) {
        uint32_t ir = mmio_read(EMMC_INTERRUPT);
        if (ir & mask) { mmio_write(EMMC_INTERRUPT, mask); return 0; }
        if (ir & INT_ERR) { mmio_write(EMMC_INTERRUPT, ir); return -1; }
        timer_delay_us(10);
    }
    return -1;
}

static int issue(uint32_t cmd, uint32_t arg, int ms)
{
    if (wait_status(SR_CMD_INHIBIT, 100) != 0) return -1;
    mmio_write(EMMC_INTERRUPT, mmio_read(EMMC_INTERRUPT));   /* clear what is pending */
    mmio_write(EMMC_ARG1, arg);
    mmio_write(EMMC_CMDTM, cmd);
    return wait_int(INT_CMD_DONE, ms);
}

/* The controller divides a base clock; the divider register splits across two fields. */
static int set_clock(uint32_t hz)
{
    uint32_t base = 41666666;         /* what the Pi's firmware gives this block */
    uint32_t div = 1;
    while (base / div > hz && div < 0x400) div <<= 1;
    uint32_t d = div >> 1;            /* the register holds half the divider */

    uint32_t c1 = mmio_read(EMMC_CONTROL1);
    c1 &= ~C1_CLK_EN;
    mmio_write(EMMC_CONTROL1, c1);
    timer_delay_us(100);

    c1 &= ~0xFFE0;                                       /* clear both divider fields */
    c1 |= ((d & 0xFF) << 8) | (((d >> 8) & 0x3) << 6);
    c1 |= C1_CLK_INTLEN;
    c1 |= (11u << 16);                                   /* data timeout, the usual value */
    mmio_write(EMMC_CONTROL1, c1);

    for (int i = 0; i < 10000; i++) {
        if (mmio_read(EMMC_CONTROL1) & C1_CLK_STABLE) break;
        timer_delay_us(10);
    }
    mmio_write(EMMC_CONTROL1, mmio_read(EMMC_CONTROL1) | C1_CLK_EN);
    timer_delay_us(100);
    return 0;
}

int emmc_init(void)
{
    card_rca = 0;
    card_is_sdhc = 0;
    card_blocks = 0;

    /* full host reset */
    mmio_write(EMMC_CONTROL0, 0);
    mmio_write(EMMC_CONTROL1, mmio_read(EMMC_CONTROL1) | C1_SRST_HC);
    for (int i = 0; i < 10000; i++) {
        if (!(mmio_read(EMMC_CONTROL1) & C1_SRST_HC)) break;
        timer_delay_us(10);
    }
    if (mmio_read(EMMC_CONTROL1) & C1_SRST_HC) { uart_puts("sd: host reset did not finish\n"); return -1; }

    set_clock(400000);                                   /* identification runs slow */
    mmio_write(EMMC_IRPT_EN, 0);
    mmio_write(EMMC_IRPT_MASK, 0xFFFFFFFF);
    mmio_write(EMMC_INTERRUPT, 0xFFFFFFFF);

    if (issue(CMD_INDEX(0), 0, 100) != 0) { uart_puts("sd: no response to reset\n"); return -1; }

    /* CMD8 says which voltages we support; a card that answers is SD version 2 or later,
     * which is what decides whether ACMD41 may ask for high capacity. */
    int v2 = issue(CMD_INDEX(8) | CMD_RSPNS_48 | CMD_CRCCHK_EN, 0x1AA, 100) == 0 &&
             (mmio_read(EMMC_RESP0) & 0xFFF) == 0x1AA;

    /* ACMD41 is repeated until the card finishes powering up. */
    for (int i = 0; i < 1000; i++) {
        if (issue(CMD_INDEX(55) | CMD_RSPNS_48 | CMD_CRCCHK_EN, 0, 100) != 0) return -1;
        uint32_t arg = 0x00FF8000 | (v2 ? (1u << 30) : 0);     /* voltage window, high capacity */
        if (issue(CMD_INDEX(41) | CMD_RSPNS_48, arg, 100) != 0) return -1;
        uint32_t r = mmio_read(EMMC_RESP0);
        if (r & (1u << 31)) {                                   /* the busy bit cleared */
            card_is_sdhc = (r & (1u << 30)) != 0;
            break;
        }
        timer_delay_ms(5);
    }

    if (issue(CMD_INDEX(2) | CMD_RSPNS_136 | CMD_CRCCHK_EN, 0, 100) != 0) {
        uart_puts("sd: card would not identify itself\n");
        return -1;
    }
    if (issue(CMD_INDEX(3) | CMD_RSPNS_48 | CMD_CRCCHK_EN, 0, 100) != 0) return -1;
    card_rca = mmio_read(EMMC_RESP0) & 0xFFFF0000;

    /* CSD tells the size; its layout differs between the two versions. */
    if (issue(CMD_INDEX(9) | CMD_RSPNS_136 | CMD_CRCCHK_EN, card_rca, 100) == 0) {
        /* A 136-bit response is stored with its CRC byte dropped, so the registers hold
         * CSD bits [39:8], [71:40], [103:72] and [127:104] -- every field below is indexed
         * through that shift rather than from the raw bit numbers in the spec. */
        uint32_t r1 = mmio_read(EMMC_RESP1);
        uint32_t r2 = mmio_read(EMMC_RESP2);
        uint32_t r3 = mmio_read(EMMC_RESP3);
        int csd_ver = (int)((r3 >> 22) & 3);                   /* CSD_STRUCTURE, bits 127:126 */
        if (csd_ver >= 1) {
            /* version 2: a plain block count, C_SIZE at bits 69:48 */
            uint32_t c_size = (r1 >> 8) & 0x3FFFFF;
            card_blocks = ((uint64_t)c_size + 1) * 1024;
        } else {
            /* version 1: size is C_SIZE, a multiplier and the read block length */
            uint32_t c_size = ((r2 & 0x3) << 10) | (r1 >> 22);     /* bits 73:62 */
            uint32_t mult   = 1u << (((r1 >> 7) & 7) + 2);          /* C_SIZE_MULT, bits 49:47 */
            uint32_t rbl    = 1u << ((r2 >> 8) & 0xF);              /* READ_BL_LEN, bits 83:80 */
            card_blocks = (uint64_t)(c_size + 1) * mult * rbl / 512;
        }
    }

    if (issue(CMD_INDEX(7) | CMD_RSPNS_48B | CMD_CRCCHK_EN, card_rca, 100) != 0) {
        uart_puts("sd: card would not select\n");
        return -1;
    }

    /* four-bit bus, then a normal clock */
    if (issue(CMD_INDEX(55) | CMD_RSPNS_48 | CMD_CRCCHK_EN, card_rca, 100) == 0 &&
        issue(CMD_INDEX(6) | CMD_RSPNS_48 | CMD_CRCCHK_EN, 2, 100) == 0)
        mmio_write(EMMC_CONTROL0, mmio_read(EMMC_CONTROL0) | (1u << 1));

    set_clock(25000000);
    mmio_write(EMMC_BLKSIZECNT, (1u << 16) | 512);
    return 0;
}

/* The data port is read a word at a time; there is no DMA path here, which is slower but
 * has no alignment or bus-address pitfalls. */
static int transfer(uint64_t lba, uint32_t count, void *buf, int write)
{
    if (count == 0) return 0;
    uint32_t *p = (uint32_t *)buf;
    uint32_t arg = card_is_sdhc ? (uint32_t)lba : (uint32_t)(lba * 512);

    if (wait_status(SR_DAT_INHIBIT, 1000) != 0) return -1;
    mmio_write(EMMC_BLKSIZECNT, (count << 16) | 512);

    uint32_t cmd = write ? CMD_INDEX(count > 1 ? 25 : 24) : CMD_INDEX(count > 1 ? 18 : 17);
    cmd |= CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IS_DATA;
    if (!write) cmd |= CMD_DAT_READ;
    if (count > 1) cmd |= CMD_DAT_MULTI | CMD_BLKCNT_EN;

    if (issue(cmd, arg, 1000) != 0) return -1;

    for (uint32_t b = 0; b < count; b++) {
        if (wait_int(write ? INT_WRITE_RDY : INT_READ_RDY, 1000) != 0) return -1;
        for (int i = 0; i < 128; i++) {
            if (write) mmio_write(EMMC_DATA, *p++);
            else *p++ = mmio_read(EMMC_DATA);
        }
    }
    if (count > 1) issue(CMD_INDEX(12) | CMD_RSPNS_48B | CMD_CRCCHK_EN, 0, 1000);
    wait_int(INT_DATA_DONE, 1000);
    return (int)count;
}

int emmc_read(uint64_t lba, uint32_t count, void *buf) { return transfer(lba, count, buf, 0); }
int emmc_write(uint64_t lba, uint32_t count, const void *buf) { return transfer(lba, count, (void *)buf, 1); }
uint64_t emmc_blocks(void) { return card_blocks; }
