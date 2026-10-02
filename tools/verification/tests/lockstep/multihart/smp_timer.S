# Each hart's own timer, and the waits, on a machine of several harts,
# lock-stepped against the multi-hart Sail driver; see
# tools/verification/lockstep_sail.py.
#
#   every hart arms its own mtimecmp at a different distance and waits in
#   WFI; each must take its timer interrupt at the instruction Sail's hart
#   does, with the clock moving per round, and faster while all wait
#   a WFI that times out with nothing pending, while another hart runs
#   hart 1 waiting in WRS.NTO on a reservation, woken when hart 0 stores to
#   the reserved word; and WRS.STO with no reservation at all
#   hart 0 writing mtime, which every hart then reads
#
# Built once per hart count, with NHARTS defined.

#ifndef NHARTS
#define NHARTS 2
#endif
.equ CLINT_MTIMECMP, 0x02004000
.equ CLINT_MTIME,    0x0200BFF8
.equ MIP_MTIP,       (1 << 7)
.equ MSTATUS_MIE,    (1 << 3)
.equ WRS_NTO,        0x00d00073
.equ WRS_STO,        0x01d00073

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
	slli t0, s0, 3
	li   s1, CLINT_MTIMECMP
	add  s1, s1, t0             # this hart's mtimecmp
	li   t0, -1
	sd   t0, 0(s1)

	# ---- a WFI with nothing to wake it: it times out -----------------------
	wfi
	csrr a1, time

	# ---- this hart's timer --------------------------------------------------
	li   t0, MIP_MTIP
	csrw mie, t0
	li   t1, CLINT_MTIME
	ld   t2, 0(t1)
	li   t0, 25
	mul  t0, t0, s0
	addi t0, t0, 20
	add  t2, t2, t0
	sd   t2, 0(s1)
	csrsi mstatus, MSTATUS_MIE
	la   s2, fired
	add  s2, s2, s0
1:	wfi
	lb   t0, 0(s2)
	beqz t0, 1b
	csrci mstatus, MSTATUS_MIE
	csrw mie, zero
	csrr a2, time

	call barrier

	# ---- WRS ---------------------------------------------------------------
	la   s3, flag
	li   t0, 1
	beq  s0, t0, waiter
	bnez s0, after_wrs
	# Hart 0: give hart 1 time to reserve, then store to the word.
	li   t0, 40
2:	addi t0, t0, -1
	bnez t0, 2b
	li   t0, 1
	sd   t0, 0(s3)
	j    after_wrs
waiter:
	lr.d t0, 0(s3)
	.word WRS_NTO
	ld   t1, 0(s3)
	beqz t1, waiter
	.word WRS_STO               # no reservation now: ends at once
after_wrs:
	call barrier

	# ---- one clock, written by hart 0 ----------------------------------------
	bnez s0, 3f
	li   t1, CLINT_MTIME
	li   t0, 0x10000
	sd   t0, 0(t1)
3:	call barrier
	csrr a3, time
	li   t1, CLINT_MTIME
	ld   a4, 0(t1)

	call barrier
	bnez s0, park
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
4:	j    4b
park:
	j    park

# Every hart arrives before any leaves. Each use has its own count.
barrier:
	la   t0, generation
	ld   t1, 0(t0)              # this barrier's number
	slli t2, t1, 3
	la   t3, counts
	add  t3, t3, t2
	li   t2, 1
	amoadd.d t2, t2, (t3)
	addi t2, t2, 1
	li   t4, NHARTS
	bne  t2, t4, 5f
	addi t1, t1, 1              # the last to arrive opens it
	sd   t1, 0(t0)
	ret
5:	ld   t2, 0(t0)
	beq  t2, t1, 5b
	ret

	.align 2
trap:
	csrr t5, mcause
	bltz t5, interrupt
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret
interrupt:
	# The timer: disarm it, and say so.
	li   t6, -1
	sd   t6, 0(s1)
	la   t6, fired
	add  t6, t6, s0
	li   t5, 1
	sb   t5, 0(t6)
	mret

.section .data
.align 6
fired:      .fill 64, 1, 0
.align 6
flag:       .dword 0
.align 6
generation: .dword 0
.align 6
counts:     .fill 16, 8, 0
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
