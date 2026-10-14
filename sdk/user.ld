/* A .t program is loaded at a fixed address and is not relocatable, so the entry stub has
   to be the first thing in the image. */
ENTRY(_start)
SECTIONS
{
    . = 0x200000;
    .text   : { KEEP(*(.text.start)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    _end = .;
}
