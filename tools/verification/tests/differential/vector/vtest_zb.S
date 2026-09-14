# Differential test for the scalar bitmanip families: Zba, Zbb, Zbs, Zicond
# and Zcb. Same contract as vtest_v.S -- results land in the
# begin_signature/end_signature region and are diffed byte-for-byte against
# spike.
#
# These were added to DoomV because a distro riscv64 userspace requires them
# (RVA23 mandates all of them), but they went in unverified: the evidence
# was only "Doom still works and Linux gets further", which cannot
# distinguish a correct implementation from one that is merely wrong in a
# way nothing exercised. Every operation below is hand-implemented arithmetic
# in ext_zb.cpp, which is exactly the code most worth checking against a
# reference.
#
# No vector state here, so unlike vtest_v.S this needs no mstatus.VS enable.

.section .text.start
.global _start
_start:
	la	sp, __stacktop
	la	s0, begin_signature	# rolling signature write pointer
	la	s1, zbdata

# Every result is a single doubleword.
.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	ld	a0, 0(s1)		# 0x0123456789abcdef
	ld	a1, 8(s1)		# 0xfedcba9876543210
	ld	a2, 16(s1)		# 0x00000000ffffffff
	ld	a3, 24(s1)		# 0x8000000000000001
	li	a4, 0			# for the clz/ctz zero cases
	li	a5, 5

	#---- Zba: shifted adds. sh3add is the one that was executing as OR.
	sh1add	t0, a0, a1
	SIGX	t0
	sh2add	t0, a0, a1
	SIGX	t0
	sh3add	t0, a0, a1
	SIGX	t0
	add.uw	t0, a0, a1		# zero-extends rs1's low half first
	SIGX	t0
	sh1add.uw t0, a0, a1
	SIGX	t0
	sh2add.uw t0, a0, a1
	SIGX	t0
	sh3add.uw t0, a0, a1
	SIGX	t0
	slli.uw	t0, a0, 7
	SIGX	t0
	slli.uw	t0, a3, 33		# shifts past bit 32 of a zero-extended value
	SIGX	t0

	#---- Zbb: logic with negation
	andn	t0, a0, a1
	SIGX	t0
	orn	t0, a0, a1
	SIGX	t0
	xnor	t0, a0, a1
	SIGX	t0

	#---- Zbb: bit counting, including the zero cases where the result is
	#---- the full width and an off-by-one is easy to hide
	clz	t0, a0
	SIGX	t0
	clz	t0, a4
	SIGX	t0
	clz	t0, a3			# MSB set -> 0
	SIGX	t0
	ctz	t0, a0
	SIGX	t0
	ctz	t0, a4
	SIGX	t0
	ctz	t0, a3			# LSB set -> 0
	SIGX	t0
	cpop	t0, a0
	SIGX	t0
	cpop	t0, a4
	SIGX	t0
	clzw	t0, a2			# W forms look at the low 32 bits only
	SIGX	t0
	clzw	t0, a4
	SIGX	t0
	ctzw	t0, a2
	SIGX	t0
	ctzw	t0, a4
	SIGX	t0
	cpopw	t0, a2
	SIGX	t0

	#---- Zbb: min/max, signed and unsigned. a1 is negative as a signed
	#---- value and huge as an unsigned one, so the two disagree.
	min	t0, a0, a1
	SIGX	t0
	minu	t0, a0, a1
	SIGX	t0
	max	t0, a0, a1
	SIGX	t0
	maxu	t0, a0, a1
	SIGX	t0

	#---- Zbb: sign and zero extension
	sext.b	t0, a0
	SIGX	t0
	sext.b	t0, a1
	SIGX	t0
	sext.h	t0, a0
	SIGX	t0
	sext.h	t0, a1
	SIGX	t0
	zext.h	t0, a1
	SIGX	t0

	#---- Zbb: rotates. The W forms rotate within 32 bits and sign-extend.
	rol	t0, a0, a5
	SIGX	t0
	ror	t0, a0, a5
	SIGX	t0
	rolw	t0, a0, a5
	SIGX	t0
	rorw	t0, a0, a5
	SIGX	t0
	rori	t0, a0, 1
	SIGX	t0
	rori	t0, a0, 63
	SIGX	t0
	roriw	t0, a0, 1
	SIGX	t0

	#---- Zbb: byte-granular
	orc.b	t0, a0
	SIGX	t0
	orc.b	t0, a2
	SIGX	t0
	rev8	t0, a0
	SIGX	t0

	#---- Zbs: single-bit ops, register and immediate forms. Shift amounts
	#---- are taken modulo 64, so 65 must behave as 1.
	bclr	t0, a0, a5
	SIGX	t0
	bclri	t0, a0, 3
	SIGX	t0
	bext	t0, a0, a5
	SIGX	t0
	bexti	t0, a0, 3
	SIGX	t0
	binv	t0, a0, a5
	SIGX	t0
	binvi	t0, a0, 3
	SIGX	t0
	bset	t0, a4, a5
	SIGX	t0
	bseti	t0, a4, 63
	SIGX	t0
	li	t1, 65
	bset	t0, a4, t1		# modulo-64 shift amount
	SIGX	t0

	#---- Zicond
	czero.eqz t0, a0, a1		# rs2 != 0 -> pass rs1 through
	SIGX	t0
	czero.eqz t0, a0, a4		# rs2 == 0 -> zero
	SIGX	t0
	czero.nez t0, a0, a1		# rs2 != 0 -> zero
	SIGX	t0
	czero.nez t0, a0, a4		# rs2 == 0 -> pass rs1 through
	SIGX	t0

	#---- Zcb: the compressed forms are aliases, but their operand
	#---- decoding is its own code path. Registers are restricted to
	#---- x8-x15 in these encodings.
	c.zext.b a0
	SIGX	a0
	ld	a0, 0(s1)
	c.sext.b a0
	SIGX	a0
	ld	a0, 0(s1)
	c.zext.h a0
	SIGX	a0
	ld	a0, 0(s1)
	c.sext.h a0
	SIGX	a0
	ld	a0, 0(s1)
	c.zext.w a0
	SIGX	a0
	ld	a0, 0(s1)
	c.not	a0
	SIGX	a0
	ld	a0, 0(s1)
	c.mul	a0, a1
	SIGX	a0
	ld	a0, 0(s1)

	#---- Zcb: byte and halfword memory access, which base RVC lacks
	la	a2, zbscratch
	c.lbu	a3, 0(s1)
	SIGX	a3
	c.lhu	a3, 0(s1)
	SIGX	a3
	c.lh	a3, 2(s1)		# sign-extending halfword load
	SIGX	a3
	c.sb	a0, 0(a2)
	c.sh	a0, 2(a2)
	ld	a3, 0(a2)		# read back what the two stores placed
	SIGX	a3

	#---- done: tohost terminates spike here; DoomV runs on to the spin
	#---- loop below, where -break stops it.
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
	.align 8
zbdata:
	.dword	0x0123456789abcdef
	.dword	0xfedcba9876543210
	.dword	0x00000000ffffffff
	.dword	0x8000000000000001
	.align 8
zbscratch:
	.fill	32, 1, 0

	.align 4
	.global begin_signature
begin_signature:
	.fill	1024, 1, 0
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
