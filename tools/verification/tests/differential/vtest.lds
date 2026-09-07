MEMORY
{
    /* Matches Memory::RAM_BASE/RAM_SIZE (src/memory.hpp) -- moved to
       0x80000000 for Stage 3 to match OpenSBI's own hardcoded, 2MB-aligned
       load address. 256MB covers code/data/bss/heap/stack for this test
       binary. WAD region starts where this ends, sized separately -- not
       part of this image. */
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
