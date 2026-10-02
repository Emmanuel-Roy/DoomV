# Harts interrupting each other through the CLINT, lock-stepped against the
# multi-hart Sail driver. Every interrupt must be taken by the same hart at
# the same instruction as in Sail's trace; see
# tools/verification/lockstep_sail.py.
#
#   hart 0 reads and writes another hart's mtimecmp, both halves and whole,
#   and reads its msip
#   hart 0 raises each other hart's machine software interrupt; each takes
#   it out of WFI, clears its own msip and answers with one back to hart 0
#   a hart's own msip, set and taken, and read through the CLINT
#
# Built once per hart count, with NHARTS defined.

#ifndef NHARTS
#define NHARTS 2
#endif
.equ CLINT_MSIP,     0x02000000
.equ CLINT_MTIMECMP, 0x02004000
.equ MIP_MSIP,       (1 << 3)
.equ MSTATUS_MIE,    (1 << 3)

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, -1
	csrw pmpaddr0, t0
	li   t0, 0x1f
	csrw pmpcfg0, t0
	csrr s0, mhartid
	li   t0, MIP_MSIP
	csrw mie, t0
	bnez s0, secondary

	# ---- hart 0 ------------------------------------------------------------
	# The next hart's CLINT registers, from here.
	li   s1, CLINT_MTIMECMP + 8
	ld   a1, 0(s1)
	li   t0, 0x123456789
	sd   t0, 0(s1)
	ld   a2, 0(s1)
	lw   a3, 0(s1)
	lw   a4, 4(s1)
	li   t0, 0x55
	sw   t0, 4(s1)
	ld   a5, 0(s1)
	li   t0, -1
	sd   t0, 0(s1)
	li   s1, CLINT_MSIP + 4
	lw   a6, 0(s1)

	# Wait for every other hart to be in its WFI loop.
	la   s1, ready
	li   t1, NHARTS - 1
1:	ld   t0, 0(s1)
	bne  t0, t1, 1b

	# One interrupt to each.
	li   s2, 1
2:	li   t1, NHARTS
	beq  s2, t1, 3f
	slli t0, s2, 2
	li   t1, CLINT_MSIP
	add  t0, t0, t1
	li   t1, 1
	sw   t1, 0(t0)
	lw   t2, 0(t0)
	addi s2, s2, 1
	j    2b

	# Their answers arrive as interrupts here. Two can land on the one msip
	# before it is taken, so the count to wait for is the senders'.
3:	csrsi mstatus, MSTATUS_MIE
	la   s1, answered_sent
	li   t1, NHARTS - 1
4:	ld   t0, 0(s1)
	bne  t0, t1, 4b
	csrci mstatus, MSTATUS_MIE

	# Its own msip: pending, read back, then taken.
	li   s1, CLINT_MSIP
	li   t0, 1
	sw   t0, 0(s1)
	csrr a1, mip
	lw   a2, 0(s1)
	csrsi mstatus, MSTATUS_MIE
	nop
	nop
	csrci mstatus, MSTATUS_MIE
	# mip.MSIP is not writable through the CSR.
	li   t0, MIP_MSIP
	csrs mip, t0
	csrr a3, mip

	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
5:	j    5b

	# ---- every other hart ----------------------------------------------------
secondary:
	csrsi mstatus, MSTATUS_MIE
	la   s1, ready
	li   t0, 1
	amoadd.d zero, t0, (s1)
6:	wfi
	j    6b

	.align 2
trap:
	csrr t5, mcause
	bltz t5, interrupt
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret
interrupt:
	# A software interrupt: clear this hart's msip, and count it.
	csrr t4, mhartid
	slli t6, t4, 2
	li   t5, CLINT_MSIP
	add  t6, t6, t5
	sw   zero, 0(t6)
	beqz t4, 7f
	# Another hart: answer hart 0.
	la   t6, answered_sent
	li   t5, 1
	amoadd.d zero, t5, (t6)
	li   t6, CLINT_MSIP
	li   t5, 1
	sw   t5, 0(t6)
	mret
7:	# Hart 0: one answer more.
	la   t6, answered
	li   t5, 1
	amoadd.d zero, t5, (t6)
	mret

.section .data
.align 6
ready:         .dword 0
.align 6
answered:      .dword 0
.align 6
answered_sent: .dword 0
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
