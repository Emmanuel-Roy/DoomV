MEMORY
{
    /* Right after the framebuffer (0x10001000 + 256K); see doomv_mmio.h.
       16MB covers code/data/bss/heap/stack. WAD region starts where this
       ends (0x11041000), sized separately -- not part of this image. */
    RAM (rwx) : ORIGIN = 0x10041000, LENGTH = 16M
}

SECTIONS
{
    .text : {
        *(.text.start)
        *(.text*)
        *(.rodata*)
    } > RAM

    .data : {
        *(.data*)
    } > RAM

    .bss : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM

    _end = .;
    _heap_start = .;
    PROVIDE(end = .); /* newlib's sbrk.c looks for this exact name */

    . = ORIGIN(RAM) + LENGTH(RAM);
    __stacktop = .;
}

ENTRY(_start)
