/*
 * Raspberry Pi entry point (AArch64).
 *
 * The GPU firmware loads kernel8.img at 0x80000 and starts every core there, so the first
 * job is to park cores 1-3 and leave one running, then drop out of EL2 (where the firmware
 * leaves us) into EL1, give it a stack, clear the BSS and call C.
 */

.section ".text.boot"
.globl _start

_start:
    /* only core 0 continues; the others wait for an address to be written for them */
    mrs     x0, mpidr_el1
    and     x0, x0, #3
    cbz     x0, core0
hang_core:
    wfe
    b       hang_core

core0:
    /* The firmware starts us at EL2 on the Pi 2 and later. Set EL1 up as 64-bit with no
     * traps, then "return" into it -- a plain branch would leave us at the wrong level and
     * every system register the kernel touches would fault. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #1
    beq     at_el1

    mov     x0, #(1 << 31)              /* EL1 is AArch64 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5                  /* EL1h, interrupts masked for now */
    msr     spsr_el2, x0
    adr     x0, at_el1
    msr     elr_el2, x0
    eret

at_el1:
    /* Let EL1 and EL0 use the floating-point and SIMD registers. Without this, the first
     * such instruction traps -- and the interpreter's object model contains them even when
     * its float support is turned off. */
    mov     x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    /* stack below the load address: the image starts at 0x80000 and grows up */
    ldr     x0, =_start
    mov     sp, x0

    /* the BSS is not in the image, so it must be zeroed before any C runs */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:
    bl      arm_main
3:  wfe
    b       3b
