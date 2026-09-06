OUTPUT_ARCH( "riscv" )
ENTRY(rvtest_entry_point)

MEMORY
{
    /* Matches Memory::RAM_BASE/RAM_SIZE (src/memory.hpp) -- see
       tools/vtest/vtest.lds for why this moved from 0x10041000. */
    RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256M
}

SECTIONS
{
  .text.init : { *(.text.init) } > RAM
  .text : { *(.text) } > RAM
  .data : { *(.data) } > RAM
  .data.string : { *(.data.string) } > RAM
  .bss : { *(.bss) } > RAM
  _end = .;
}
