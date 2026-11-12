/* DoomV guest memory map: RAM at 0x80000000 (DoomV's Memory::RAM_BASE). The
   WAD is placed by DoomV above guest RAM and found through MMIO_WAD_BASE.
   (After DoomV's tools/doom/doombuild/riscv.lds.) */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64M
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
        __global_pointer$ = . + 0x800;
        *(.sdata*)
    } > RAM

    /* .sbss is a separate name from .bss: list both, or the heap overlaps
       globals (DoomV's riscv.lds learned this the hard way). */
    .bss : {
        _sbss = .;
        *(.sbss*)
        *(.bss*)
        *(.scommon)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    } > RAM

    _end = .;
    PROVIDE(end = .);

    . = ORIGIN(RAM) + LENGTH(RAM);
    __stacktop = .;
}

ENTRY(_start)
