/* The GPU firmware loads kernel8.img at 0x80000 and jumps to its first byte, so the entry
   code has to be placed first in the image. */
ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text : {
        KEEP(*(.text.boot))
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }

    . = ALIGN(8);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    }
    . = ALIGN(8);
    __bss_end = .;

    _end = .;
}
