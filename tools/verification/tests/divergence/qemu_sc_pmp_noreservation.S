# sc.w with no reservation, to a doubleword a locked PMP entry denies
# (QEMU differs; whether the spec allows it is not settled here).
#
# PMP entry 1 is locked (L) with no permissions over blk, so even M-mode may
# not store there. With no LR before it, the sc.w can only fail. Sail checks
# the access first and raises store/AMO access-fault (7); Spike, Whisper
# and DoomV do the same.
#
# QEMU 10.2 sees there is no reservation, fails the SC (rd = 1) and never
# checks PMP, so nothing traps.
#
# Found by riscv-arch-test PMPZalrsc_cfg_wr-00.
#
#! qemu: trap sc.w

.option arch, +zalrsc

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	la   t0, blk
	srli t0, t0, 2
	csrw pmpaddr0, t0              # entry 0: off; the bottom of entry 1
	la   t0, blk + 16
	srli t0, t0, 2
	csrw pmpaddr1, t0
	li   t0, 0x8800                # entry 1: L, TOR, no R/W/X
	csrw pmpcfg0, t0

	la   s0, blk
	li   a7, 1
	li   t5, 0
	sc.w t1, a7, (s0)              # Sail 7; QEMU fails it, no trap
	mv   a0, t5
	mv   a1, t1

	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

.section .data
.align 4
blk:      .dword 0, 0
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
