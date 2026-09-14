/* Linker script for the V differential test. Mirrors tools/vtest/vtest.lds
   (RAM at 0x80000000, matching Memory::RAM_BASE) but additionally keeps a
   .tohost section, which spike's HTIF needs to see in order to terminate.
   DoomV ignores it -- it halts on the spin loop via -break instead. */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256M
}

SECTIONS
{
    .text : {
        *(.text.start)
        *(.text*)
        *(.rodata*)
        *(.srodata*)
    } > RAM

    .data : {
        *(.data*)
        *(.sdata*)
    } > RAM

    /* Kept separate and never discarded: spike locates HTIF purely by the
       tohost/fromhost symbols, so folding this into .data would still work
       but makes the contract less obvious. */
    .tohost : {
        *(.tohost)
    } > RAM

    .bss : {
        _sbss = .;
        *(.bss*)
        *(.sbss*)
        *(.scommon)
        *(COMMON)
        _ebss = .;
    } > RAM

    _end = .;
    . = ORIGIN(RAM) + LENGTH(RAM);
    __stacktop = .;
}

ENTRY(_start)
