/*
 * The exception vector table.
 *
 * AArch64 wants 16 entries of 128 bytes each, grouped by where the exception came from.
 * Only the EL1 cases matter here: nothing runs at EL0 yet, and there is no EL2 to return
 * to. Each entry saves the registers a C handler may clobber, calls it, and restores them
 * -- an interrupt that corrupted a register would surface as a fault somewhere unrelated.
 */

.macro SAVE_REGS
    sub     sp, sp, #256
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]
.endm

.macro RESTORE_REGS
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #256
.endm

.macro VECTOR label
    .align  7
    b       \label
.endm

.align 11
.globl vector_table
vector_table:
    /* current EL, SP0 -- not used, but a fault here must still be reported */
    VECTOR  el1_invalid
    VECTOR  el1_invalid
    VECTOR  el1_invalid
    VECTOR  el1_invalid

    /* current EL, SPx -- where the kernel's own exceptions land */
    VECTOR  el1_sync
    VECTOR  el1_irq
    VECTOR  el1_invalid
    VECTOR  el1_invalid

    /* lower EL, AArch64 */
    VECTOR  el1_invalid
    VECTOR  el1_irq
    VECTOR  el1_invalid
    VECTOR  el1_invalid

    /* lower EL, AArch32 */
    VECTOR  el1_invalid
    VECTOR  el1_invalid
    VECTOR  el1_invalid
    VECTOR  el1_invalid

el1_sync:
    SAVE_REGS
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      arm_sync_handler
    RESTORE_REGS
    eret

el1_irq:
    SAVE_REGS
    bl      arm_irq_handler
    RESTORE_REGS
    eret

el1_invalid:
    SAVE_REGS
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      arm_invalid_handler
    RESTORE_REGS
    eret

.globl arm_vectors_install
arm_vectors_install:
    adr     x0, vector_table
    msr     vbar_el1, x0
    isb
    ret

.globl arm_irq_enable
arm_irq_enable:
    msr     daifclr, #2            /* unmask IRQs */
    ret

.globl arm_irq_disable
arm_irq_disable:
    msr     daifset, #2
    ret
