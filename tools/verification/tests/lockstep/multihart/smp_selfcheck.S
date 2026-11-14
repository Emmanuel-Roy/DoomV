# Several harts that check their own result, so that a simulator with no
# lock-step against Sail -- Spike -p, Whisper --harts, QEMU -smp (MTTCG) --
# can be run on it too (tools/verification/divergence.py --multihart).
# Lock-stepped against Sail like the others here as well.
#
#   1. every hart adds 1 to a shared doubleword ITER times with amoadd.d
#   2. every hart adds 1 to another ITER times with an LR/SC loop
#   3. ABA: hart 0 takes a reservation (lr.d); hart 1 then stores a new
#      value to the reserved doubleword and stores the old one back; hart 0's
#      sc.d must fail. "An SC may succeed only if no store from another hart
#      to the reservation set can be observed to have occurred between the
#      LR and the SC" (unprivileged spec, Zalrsc) -- the value being the
#      same again does not matter. An SC done as a compare-and-swap of the
#      value LR read succeeds.
#
# Hart 0 waits for all, checks, and writes tohost: 1 if all is well, else
# (code << 1) | 1 with code 1 (the AMO count), 2 (the LR/SC count) or 3 (the
# ABA SC succeeded). Built once per hart count, with NHARTS defined.

#ifndef NHARTS
#define NHARTS 2
#endif
#ifndef ITER
#define ITER 200
#endif

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

	# ---- 1. AMO count -------------------------------------------------------
	la   s1, amo_count
	li   s2, ITER
	li   t0, 1
1:	amoadd.d zero, t0, (s1)
	addi s2, s2, -1
	bnez s2, 1b

	# ---- 2. LR/SC count -----------------------------------------------------
	la   s1, lrsc_count
	li   s2, ITER
2:	lr.d t1, (s1)
	addi t1, t1, 1
	sc.d t2, t1, (s1)
	bnez t2, 2b
	addi s2, s2, -1
	bnez s2, 2b

	# ---- 3. ABA, harts 0 and 1 ----------------------------------------------
#if NHARTS > 1
	li   t0, 1
	beqz s0, aba0
	beq  s0, t0, aba1
	j    arrive

aba0:
	la   s1, aba
	la   s2, flag
	la   s3, ack
	lr.d t1, (s1)                  # the reservation
	li   t0, 1
	sd   t0, 0(s2)                 # tell hart 1 (another line: kept)
3:	ld   t0, 0(s3)
	beqz t0, 3b                    # until hart 1 has stored twice
	addi t1, t1, 1
	sc.d t2, t1, (s1)              # must fail: t2 != 0
	la   s1, aba_sc
	sd   t2, 0(s1)
	j    arrive

aba1:
	la   s1, aba
	la   s2, flag
	la   s3, ack
4:	ld   t0, 0(s2)
	beqz t0, 4b
	ld   t1, 0(s1)
	addi t2, t1, 5
	sd   t2, 0(s1)                 # a new value ...
	sd   t1, 0(s1)                 # ... and the old one again
	li   t0, 1
	sd   t0, 0(s3)
#endif

	# ---- arrive; hart 0 checks ----------------------------------------------
arrive:
	la   s1, arrived
	li   t0, 1
	amoadd.d zero, t0, (s1)
	bnez s0, park
5:	ld   t0, 0(s1)
	li   t1, NHARTS
	bne  t0, t1, 5b

	li   t3, NHARTS * ITER
	li   a0, 1
	la   s1, amo_count
	ld   t0, 0(s1)
	bne  t0, t3, fail
	li   a0, 2
	la   s1, lrsc_count
	ld   t0, 0(s1)
	bne  t0, t3, fail
#if NHARTS > 1
	li   a0, 3
	la   s1, aba_sc
	ld   t0, 0(s1)
	beqz t0, fail
#endif
	li   t0, 1
	j    report
fail:
	slli t0, a0, 1
	ori  t0, t0, 1
report:
	la   t1, tohost
	sd   t0, 0(t1)
6:	j    6b

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
amo_count:  .dword 0
.align 6
lrsc_count: .dword 0
.align 6
aba:        .dword 0x5a5a
.align 6
flag:       .dword 0
.align 6
ack:        .dword 0
.align 6
aba_sc:     .dword 0
.align 6
arrived:    .dword 0
.align 3
.global tohost
tohost:     .dword 0
.global fromhost
fromhost:   .dword 0
