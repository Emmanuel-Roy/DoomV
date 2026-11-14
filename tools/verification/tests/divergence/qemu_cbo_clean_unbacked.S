# cbo.clean through a valid PTE to physical memory that is not there
# (QEMU bug).
#
# The leaf for VA 0x40000000 is valid (V R W X A D) but points at physical
# address 0, where the platform has no memory. A CMO is checked like a
# store for exception purposes: Sail raises store/AMO access-fault (7);
# Spike, Whisper and DoomV do the same.
#
# QEMU 10.2 retires the cbo.clean: it checks the translation and the
# permissions only, never whether anything is there.
#
# Found by riscv-arch-test sv39_zicbom_exceptions_Smode / _Umode.
#
#! qemu: trap cbo.clean

.option arch, +zicbom

.equ PTE_LEAF,  0xCF
.equ VA,        0x40000000

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, -1
	csrw pmpaddr0, t0
	li   t0, 0x1f
	csrw pmpcfg0, t0
	csrw medeleg, zero
	li   t0, 0xf0
	csrw menvcfg, t0               # CBIE, CBCFE, CBZE: S-mode may use them

	la   s1, root
	li   t0, PTE_LEAF              # root[1]: VA 0x40000000 -> PA 0
	sd   t0, 8(s1)
	li   t0, 0x80000000
	srli t0, t0, 12
	slli t0, t0, 10
	ori  t0, t0, PTE_LEAF
	sd   t0, 16(s1)                # root[2]: 1 GB identity leaf
	la   t0, root
	srli t0, t0, 12
	li   t1, (8 << 60)
	or   t0, t0, t1
	csrw satp, t0
	sfence.vma
	li   t0, (3 << 11)
	csrc mstatus, t0
	li   t0, (1 << 11)
	csrs mstatus, t0               # MPP = S
	la   t0, super
	csrw mepc, t0
	mret

super:
	li   s0, VA
	cbo.clean (s0)                 # Sail 7; QEMU retires
	mv   a0, t5
	ecall

done:
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	li   t6, 9
	beq  t5, t6, done
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

.section .data
.align 12
root:     .fill 512, 8, 0
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
