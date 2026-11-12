// Reset entry for the DoomV guest (after DoomV's own doombuild/start.S).
	.section .text.start
	.global _start
_start:
	.option push
	.option norelax
	la gp, __global_pointer$
	.option pop
	la sp, __stacktop

	// Count retired instructions (minstret) from the start, as the Pico does.
	csrw mcountinhibit, zero

	// .data arrives initialised via the ELF loader; clear .bss.
	la a0, _sbss
	la a1, _ebss
	bge a0, a1, 2f
1:	sw zero, 0(a0)
	addi a0, a0, 4
	blt a0, a1, 1b
2:
	call main

	.global _exit
_exit:
	j _exit
