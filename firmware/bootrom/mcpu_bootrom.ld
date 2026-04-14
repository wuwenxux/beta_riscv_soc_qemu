/* Linker script for Beta MCPU BootROM test binary
 * BootROM is at 0x1000, size 12KB max */
OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS {
    . = 0x00001000;
    .text   : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) }
    /DISCARD/ : { *(.eh_frame) *(.comment) }
}
