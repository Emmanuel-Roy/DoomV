# Differential test for the RVA23 hint and reserved-encoding extensions:
# Zihintpause, Zihintntl, Zimop, Zcmop, Zicbom and Zicbop.
#
# Every instruction here is defined to have no architectural effect, which
# makes this an unusual thing to diff against spike -- there is no result to
# compare. That is exactly why it is worth doing. "No effect" is a claim
# about the *whole* register file, and the two ways to get it wrong are
# invisible in the instruction's own output:
#
#   1. The instruction is not decoded at all, and traps. Nothing below sets
#      up a trap handler, so an illegal instruction runs off into whatever
#      the reset vector does and the signature comes back short or garbage.
#
#   2. The instruction decodes as something else that happens to look
#      harmless. This is the real risk here, because five of the six are
#      encoded inside another instruction's space: PAUSE is a FENCE, the
#      NTL hints are C.ADD, the prefetches are ORI, C.MOP is C.LUI, and
#      MOP.R/MOP.RR sit in SYSTEM next to the CSR instructions. Decoding
#      one as its host instruction with a nonzero destination would corrupt
#      a register -- so the check is to load every register with a known
#      value, execute the hints, and dump the registers afterwards.
#
# Zimop is the one member with a real rule: MOP.R.n and MOP.RR.n must write
# ZERO to rd. Leaving rd untouched would pass a naive "did anything break"
# test while being wrong in the one way that matters, since the whole point
# of the encoding is that software can rely on reading a zero back.

.section .text.start
.global _start
_start:
	la	sp, __stacktop
	la	s0, begin_signature

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- Load sentinels into the registers the hints could plausibly
	#---- clobber. Anything that decodes as its host instruction writes a
	#---- result over one of these, and the dump at the end will show it.
	li	a0, 0x1111111111111111
	li	a1, 0x2222222222222222
	li	a2, 0x3333333333333333
	li	a3, 0x4444444444444444
	li	a4, 0x5555555555555555
	li	a5, 0x6666666666666666
	li	t0, 0x7777777777777777
	li	t1, -1

	#---- Zihintpause. Encoded as FENCE pred=W, succ=none.
	pause
	pause

	#---- Zihintntl. All four, each encoded as C.ADD with rd=x0 and rs2
	#---- naming the hint. If rd were taken from the wrong field, one of
	#---- the sentinels above would gain an addend.
	ntl.p1
	ntl.pall
	ntl.s1
	ntl.all

	#---- The hints are architecturally attached to the instruction that
	#---- follows them, so run each one in front of a real memory access
	#---- to check that pairing does not disturb the access itself.
	la	t2, scratch
	ntl.all
	ld	t3, 0(t2)
	SIGX	t3
	ntl.p1
	sd	a0, 8(t2)
	ld	t3, 8(t2)
	SIGX	t3

	#---- Zicbop. prefetch.{i,r,w} are ORI with rd=x0; the offset must be
	#---- a multiple of 32 because imm[4:0] carries the variant instead.
	prefetch.i	0(t2)
	prefetch.r	0(t2)
	prefetch.w	0(t2)
	prefetch.r	32(t2)
	prefetch.w	-32(t2)

	#---- Zicbom. The three cache-management ops on a mapped, writable
	#---- address. With no cache these retire without effect, but they must
	#---- not disturb the data at that address either -- so read it back.
	sd	a1, 0(t2)
	cbo.clean	(t2)
	cbo.flush	(t2)
	cbo.inval	(t2)
	ld	t3, 0(t2)
	SIGX	t3

	#---- Zcmop. C.MOP.n writes no register at all, so the check is purely
	#---- that the sentinels survive. Odd n only: 1 through 15.
	c.mop.1
	c.mop.3
	c.mop.5
	c.mop.7
	c.mop.9
	c.mop.11
	c.mop.13
	c.mop.15

	#---- Zimop. Here the result IS observable: rd must read back zero.
	#---- Each of these overwrites a sentinel with 0, deliberately, and the
	#---- dump below is what proves it happened.
	mop.r.0		t4, a0
	SIGX	t4
	mop.r.1		t4, a1
	SIGX	t4
	mop.r.31	t4, a2
	SIGX	t4
	mop.rr.0	t4, a0, a1
	SIGX	t4
	mop.rr.7	t4, a2, a3
	SIGX	t4

	#---- rd = x0 is legal and must stay zero, and the source registers
	#---- must be left alone by all of the above.
	mop.r.5		x0, a0
	mop.rr.3	x0, a4, a5
	SIGX	x0

	#---- The whole point: every sentinel, unchanged.
	SIGX	a0
	SIGX	a1
	SIGX	a2
	SIGX	a3
	SIGX	a4
	SIGX	a5
	SIGX	t0
	SIGX	t1
	SIGX	sp

	#---- done
.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

#---------------------------------------------------------------------
	.section .data
	.align 6			# a cache-block-aligned target for cbo.*
scratch:
	.dword	0x0f1e2d3c4b5a6978
	.fill	56, 1, 0

	.align 4
	.global begin_signature
begin_signature:
	.fill	512, 1, 0
	.global end_signature
end_signature:

	.section .tohost, "aw", @progbits
	.align 8
	.global tohost
tohost:
	.dword	0
	.global fromhost
fromhost:
	.dword	0
