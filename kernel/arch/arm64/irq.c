#include "irq.h"
#include "bcm.h"
#include "uart.h"

void loader_return(void);

/*
 * The ARM generic timer drives the tick. Its frequency is reported by the core
 * (19.2 MHz on a Pi 2/3, 54 MHz on a Pi 4, and whatever the firmware set on a Pi 5), so
 * nothing here assumes a rate.
 */

static volatile uint64_t ticks;
static uint64_t tick_interval;
static uint64_t timer_freq;

/* Which interrupt controller this board has. The Pi 4 and 5 have a GIC; before that
 * Broadcom's own controller sat in the peripheral block. */
static int use_gic;

#define BCM_IRQ_BASE      (bcm_periph_base + 0xB200)
#define BCM_IRQ_ENABLE_1  (BCM_IRQ_BASE + 0x10)
/* The per-core timer control on the Pi 2/3 lives in its own block above the peripherals. */
#define LOCAL_BASE        0x40000000
#define LOCAL_TIMER_CTRL  (LOCAL_BASE + 0x40)     /* core 0 */

#define GIC_DIST   (bcm_periph_base + 0x1841000)
#define GIC_CPU    (bcm_periph_base + 0x1842000)

static uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static uint64_t read_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* The next deadline is an absolute counter value, not a delay: setting a delay each time
 * would add the handler's own latency to every interval, and the clock would run slow. */
static uint64_t next_deadline;

static void arm_deadline(uint64_t at)
{
    next_deadline = at;
    __asm__ volatile("msr cntp_cval_el0, %0" :: "r"(at));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1));   /* enabled, not masked */
}

void irq_init(void)
{
    arm_vectors_install();
    use_gic = (bcm_generation >= 4);     /* the Pi 4 and 5 have a GIC; before that, Broadcom's own */

    if (use_gic) {
        /* GIC-400: enable the distributor and this CPU interface, and let every priority
         * through. The firmware has already routed the timer interrupt to this core. */
        mmio_write(GIC_DIST + 0x000, 1);
        mmio_write(GIC_CPU + 0x004, 0xFF);        /* priority mask: allow all */
        mmio_write(GIC_CPU + 0x000, 1);
        mmio_write(GIC_DIST + 0x100, 0xFFFFFFFF); /* enable the first 32 (PPIs/SGIs) */
    } else {
        /* Route the physical timer's interrupt on core 0 to IRQ rather than FIQ. */
        mmio_write(LOCAL_TIMER_CTRL, 1 << 1);     /* CNTPNSIRQ -> IRQ */
    }
}

void timer_init(uint32_t hz)
{
    timer_freq = read_cntfrq();
    if (timer_freq == 0) timer_freq = 19200000;   /* the Pi 2/3 rate, if the core will not say */
    tick_interval = timer_freq / (hz ? hz : 100);
    ticks = 0;
    arm_deadline(read_cntpct() + tick_interval);
    arm_irq_enable();
}

uint64_t timer_ticks(void) { return ticks; }

uint64_t timer_us(void)
{
    uint64_t f = timer_freq ? timer_freq : read_cntfrq();
    if (!f) return 0;
    return read_cntpct() * 1000000ull / f;
}

void timer_delay_us(uint64_t us)
{
    uint64_t f = timer_freq ? timer_freq : read_cntfrq();
    if (!f) { bcm_delay((int32_t)(us * 100)); return; }
    uint64_t end = read_cntpct() + (us * f) / 1000000ull;
    while (read_cntpct() < end) { }
}

void timer_delay_ms(uint64_t ms) { timer_delay_us(ms * 1000); }

/* ---- the handlers the vector table calls ---- */

void arm_irq_handler(void)
{
    /* Only the timer is wired up so far; its interrupt is acknowledged by setting the next
     * deadline, which is what clears the condition. */
    uint64_t ctl;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(ctl));
    if (ctl & (1 << 2)) {                          /* the status bit: the timer fired */
        /* Advance by whole intervals from the deadline that just passed. If the kernel was
         * busy for longer than one interval, that is counted rather than silently lost. */
        uint64_t now = read_cntpct();
        do {
            ticks++;
            next_deadline += tick_interval;
        } while (next_deadline <= now);
        arm_deadline(next_deadline);
    }
    if (use_gic) {
        uint32_t iar = mmio_read(GIC_CPU + 0x00C);
        if ((iar & 0x3FF) != 0x3FF) mmio_write(GIC_CPU + 0x010, iar);   /* end of interrupt */
    }
}

static void report(const char *what, uint64_t esr, uint64_t elr, uint64_t far)
{
    uart_puts("\n*** ");
    uart_puts(what);
    uart_puts("\n    ESR ");
    uart_put_hex(esr);
    uart_puts("  ELR ");
    uart_put_hex(elr);
    uart_puts("  FAR ");
    uart_put_hex(far);
    uart_puts("\n");
}

void arm_sync_handler(uint64_t esr, uint64_t elr, uint64_t far)
{
    report("synchronous exception", esr, elr, far);
    for (;;) __asm__ volatile("wfe");
}

/* A program that faults is ended; the board carries on, which is the whole point of
 * running it at EL0 in its own window of memory. */
void arm_user_fault(uint64_t esr, uint64_t elr, uint64_t far)
{
    report("the program faulted and was stopped", esr, elr, far);
    loader_return();
}

void arm_invalid_handler(uint64_t esr, uint64_t elr, uint64_t far)
{
    report("unexpected exception", esr, elr, far);
    for (;;) __asm__ volatile("wfe");
}
