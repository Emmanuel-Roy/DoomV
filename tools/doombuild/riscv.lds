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
        *(.srodata*)
    } > RAM

    .data : {
        *(.data*)
        *(.sdata*)
    } > RAM

    /* *(.bss*) alone doesn't catch .sbss* -- different name, not a suffix
       match. GCC/newlib emit a lot of small-data .sbss.* sections (see
       PLAN.md); missing them here meant the linker orphan-placed them
       *after* _end/heap_start below, so the heap silently overlapped
       still-live globals (drone, loop_interface, gametic, ...) from the
       very first malloc call. Found by an actual corrupted-global chase,
       not by inspection -- see PLAN.md's debugging notes. */
    .bss : {
        _sbss = .;
        *(.bss*)
        *(.sbss*)
        *(.scommon)
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
