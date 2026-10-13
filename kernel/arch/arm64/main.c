#include "bcm.h"
#include "uart.h"
#include "mailbox.h"
#include "version.h"

/* What the firmware hands over, printed so a serial console shows the port is alive and
 * the board was identified before anything harder is attempted. */
static void banner(void)
{
    uart_puts("\n");
    uart_puts("  ArmtOS " TOS_VERSION " -- tOS on ARM\n");
    uart_puts("  board: ");
    uart_puts(bcm_model_name());
    uart_puts("\n  peripherals at ");
    uart_put_hex(bcm_periph_base);
    uart_puts("\n");
}

void arm_main(void)
{
    bcm_detect();
    uart_init();
    banner();

    uint32_t rev = 0, sz = 0;
    if (mailbox_board_revision(&rev) == 0) {
        uart_puts("  board revision: ");
        uart_put_hex(rev);
        uart_puts("\n");
    }
    if (mailbox_arm_memory(&sz) == 0) {
        uart_puts("  memory: ");
        uart_put_dec(sz / (1024 * 1024));
        uart_puts(" MB\n");
    }

    uart_puts("\n[OK] early boot reached C\n");
    uart_puts("ArmtOS> ");
    for (;;) {
        int c = uart_getc_nonblock();
        if (c < 0) { __asm__ volatile("wfe"); continue; }
        if (c == '\r' || c == '\n') uart_puts("\nArmtOS> ");
        else uart_putc((char)c);
    }
}
