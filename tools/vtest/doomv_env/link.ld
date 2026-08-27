OUTPUT_ARCH( "riscv" )
ENTRY(rvtest_entry_point)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x10041000, LENGTH = 16M
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
