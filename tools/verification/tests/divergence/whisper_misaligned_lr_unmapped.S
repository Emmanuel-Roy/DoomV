# A misaligned LR to an unmapped page: access fault or page fault
# (allowed either way).
#
# Sail checks a misaligned LR/AMO before translating it, and raises the
# access fault its config asks for (load access-fault, 5, for LR; store/AMO
# access-fault, 7, for an AMO); DoomV and Spike do the same. Whisper
# translates first and raises the page fault (13, 15). The privileged spec
# lets address-misaligned exceptions -- and an access fault raised in place
# of one -- take either priority against page faults, so neither is wrong;
# the difference is recorded so a change in either shows.
#
# (QEMU differs too: it raises address-misaligned, 4, for both -- allowed
# for the LR, its bug for the AMO: qemu_amo_misaligned_cause.S.)
#
# Found by riscv-arch-test sv39_exceptions_Zalrsc_Mmode.
#
#! whisper: x30 csrr
#! qemu: x30 csrr

.option arch, +zaamo, +zalrsc

.equ PTE_LEAF,  0xCF
.equ PTE_TABLE, 0x01
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

	la   s1, root
	li   t0, 0x80000000
	srli t0, t0, 12
	slli t0, t0, 10
	ori  t0, t0, PTE_LEAF
	sd   t0, 16(s1)                # root[2]: 1 GB identity leaf; VA unmapped
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
	li   s0, VA + 2
	li   a7, 1
	lr.w t1, (s0)                  # Sail 5, Whisper 13
	mv   a0, t5
	amoadd.w t1, a7, (s0)          # Sail 7, Whisper 15
	mv   a1, t5
	li   s0, VA
	lr.w t1, (s0)                  # aligned: a page fault on all of them
	mv   a2, t5
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
