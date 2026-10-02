# Several harts on shared memory, lock-stepped against the multi-hart Sail
# driver. Nothing here checks a value itself: every register write, store and
# store-conditional result is compared with Sail's trace, so the test only
# has to make the harts contend. See tools/verification/lockstep_sail.py.
#
#   AMOs from every hart to the same words (the values each sees depend on
#   the interleaving, which the two must agree on)
#   LR/SC increment loops on one word, where a hart's SC fails because
#   another hart's store ended its reservation
#   an SC to another address in the reserved set, and one after a store by
#   the same hart (which leaves the reservation alone)
#
# Built once per hart count, with NHARTS defined. Every hart starts here with
# a0 = mhartid; hart 0 ends the test once all have arrived.

#ifndef NHARTS
#define NHARTS 2
#endif
.equ ITER, 12

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

	# ---- AMOs on shared words ----------------------------------------------
	la   s1, counter
	li   s2, ITER
1:	li   t0, 1
	amoadd.d t1, t0, (s1)
	addi t3, s1, 8
	amoadd.w t2, s0, (t3)
	addi s2, s2, -1
	bnez s2, 1b

	la   s1, mixed
	addi t0, s0, 1
	amoswap.d t1, t0, (s1)
	addi t3, s1, 8
	amomax.d t1, t0, (t3)
	addi t3, s1, 16
	amominu.d t1, t0, (t3)
	addi t3, s1, 24
	amoor.w t1, t0, (t3)
	addi t3, s1, 28
	amoxor.w t1, t0, (t3)
	addi t3, s1, 32
	amoand.d t1, t0, (t3)
	neg  t0, t0
	addi t3, s1, 40
	amomin.d t1, t0, (t3)
	addi t3, s1, 48
	amomaxu.w t1, t0, (t3)

	# ---- LR/SC increments of one word, all harts at once -------------------
	la   s1, lrsc
	li   s2, ITER
2:	lr.d t1, (s1)
	addi t1, t1, 1
	sc.d t2, t1, (s1)
	bnez t2, 2b
	addi s2, s2, -1
	bnez s2, 2b

	# The same with a word-sized pair and a little distance between them.
	li   s2, ITER
3:	addi t3, s1, 8
	lr.w t1, (t3)
	addi t1, t1, 3
	add  t3, t1, s0
	addi t3, s1, 8
	sc.w t2, t1, (t3)
	bnez t2, 3b
	addi s2, s2, -1
	bnez s2, 3b

	# ---- the reservation set ------------------------------------------------
	# Each hart on its own doubleword, so only its own accesses matter.
	la   s1, own
	slli t0, s0, 4
	add  s1, s1, t0
	lr.w t1, (s1)
	addi t3, s1, 4
	sc.w t2, t0, (t3) # another address, same 8-byte set
	lr.w t1, (s1)
	sw   s0, 4(s1)            # this hart's own store: the reservation stays
	sc.w t2, t0, (s1)
	lr.d t1, (s1)
	addi t3, s1, 8
	sc.d t2, t0, (t3) # the next set: fails
	sc.d t2, t0, (s1) # no reservation left: fails

	# ---- arrive, and hart 0 ends it ----------------------------------------
	la   s1, arrived
	li   t0, 1
	amoadd.d zero, t0, (s1)
	bnez s0, park
4:	ld   t0, 0(s1)
	li   t1, NHARTS
	bne  t0, t1, 4b
	la   s1, counter
	ld   a1, 0(s1)
	lw   a2, 8(s1)
	la   s1, lrsc
	ld   a3, 0(s1)
	lw   a4, 8(s1)
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
5:	j    5b

park:
	j    park

	.align 2
trap:
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

.section .data
.align 6
counter:  .dword 0, 0
.align 6
mixed:    .dword 0, 0, -1, 0, -1, 0, 0
.align 6
lrsc:     .dword 0, 0
.align 6
own:      .fill 16 * 16, 1, 0
.align 6
arrived:  .dword 0
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
