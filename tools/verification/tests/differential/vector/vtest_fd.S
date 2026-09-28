# Differential test for the F and D extensions.
#
# Written before splitting ext_fd.cpp into per-extension files, precisely so
# the split has something to be checked against: F/D had no coverage in this
# harness at all, and a 60-case interleaved funct7 switch is the last place
# to attempt a refactor blind.
#
# Every result is dumped as raw bits (via fmv.x.w / fmv.x.d) rather than as
# a number, so NaN payloads and signed zeros compare exactly. That matters
# here more than usual: this project has already had two FP bugs of exactly
# that shape -- NaN payloads leaking from the host FPU instead of being
# canonicalised, and an out-of-range float-to-unsigned conversion.
#
# fcvt.s.d and fcvt.d.s are deliberately included: they involve both widths,
# and the spec lists them under D, so they are the natural place for a
# split between the two files to go wrong.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	# mstatus.FS must be on or every instruction below is illegal -- DoomV
	# enforces this now, and spike always did.
	li	t0, 1 << 13
	csrs	mstatus, t0

	la	s0, begin_signature
	la	s1, fdata

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm
.macro SIGD freg			# double, as raw bits
	fmv.x.d	t6, \freg
	SIGX	t6
.endm
.macro SIGS freg			# single, as raw bits
	fmv.x.w	t6, \freg
	SIGX	t6
.endm

	#---- loads (also the values everything below uses)
	fld	fa0, 0(s1)		# 1.5
	fld	fa1, 8(s1)		# -2.25
	fld	fa2, 16(s1)		# 0.0
	fld	fa3, 24(s1)		# -0.0
	fld	fa4, 32(s1)		# +inf
	fld	fa5, 40(s1)		# quiet NaN
	flw	ft0, 48(s1)		# 1.5f
	flw	ft1, 52(s1)		# -2.25f
	SIGD	fa0
	SIGD	fa1
	SIGS	ft0
	SIGS	ft1

	#---- double arithmetic
	fadd.d	ft2, fa0, fa1
	SIGD	ft2
	fsub.d	ft2, fa0, fa1
	SIGD	ft2
	fmul.d	ft2, fa0, fa1
	SIGD	ft2
	fdiv.d	ft2, fa0, fa1
	SIGD	ft2
	fsqrt.d	ft2, fa0
	SIGD	ft2

	#---- single arithmetic
	fadd.s	ft3, ft0, ft1
	SIGS	ft3
	fsub.s	ft3, ft0, ft1
	SIGS	ft3
	fmul.s	ft3, ft0, ft1
	SIGS	ft3
	fdiv.s	ft3, ft0, ft1
	SIGS	ft3
	fsqrt.s	ft3, ft0
	SIGS	ft3

	#---- min/max, including the signed-zero and NaN cases where the rule
	#---- is not simply "compare and pick"
	fmin.d	ft2, fa0, fa1
	SIGD	ft2
	fmax.d	ft2, fa0, fa1
	SIGD	ft2
	fmin.d	ft2, fa2, fa3		# +0.0 vs -0.0
	SIGD	ft2
	fmax.d	ft2, fa2, fa3
	SIGD	ft2
	fmin.d	ft2, fa0, fa5		# a NaN operand
	SIGD	ft2
	fmin.s	ft3, ft0, ft1
	SIGS	ft3
	fmax.s	ft3, ft0, ft1
	SIGS	ft3

	#---- sign injection
	fsgnj.d	ft2, fa0, fa1
	SIGD	ft2
	fsgnjn.d ft2, fa0, fa1
	SIGD	ft2
	fsgnjx.d ft2, fa0, fa1
	SIGD	ft2
	fsgnj.s	ft3, ft0, ft1
	SIGS	ft3

	#---- comparisons produce integer results
	feq.d	t0, fa0, fa1
	SIGX	t0
	flt.d	t0, fa0, fa1
	SIGX	t0
	fle.d	t0, fa0, fa1
	SIGX	t0
	feq.d	t0, fa5, fa5		# NaN is not equal to itself
	SIGX	t0
	flt.s	t0, ft0, ft1
	SIGX	t0

	#---- fclass: a bitmask, so a wrong class shows up as a wrong bit
	fclass.d t0, fa0
	SIGX	t0
	fclass.d t0, fa3		# -0.0
	SIGX	t0
	fclass.d t0, fa4		# +inf
	SIGX	t0
	fclass.d t0, fa5		# quiet NaN
	SIGX	t0
	fclass.s t0, ft1
	SIGX	t0

	#---- conversions to and from integers, both widths and both signs
	fcvt.w.d  t0, fa1
	SIGX	t0
	fcvt.wu.d t0, fa0
	SIGX	t0
	fcvt.l.d  t0, fa1
	SIGX	t0
	fcvt.lu.d t0, fa0
	SIGX	t0
	li	t1, -7
	fcvt.d.w  ft2, t1
	SIGD	ft2
	fcvt.d.wu ft2, t1
	SIGD	ft2
	fcvt.d.l  ft2, t1
	SIGD	ft2
	fcvt.s.w  ft3, t1
	SIGS	ft3
	fcvt.w.s  t0, ft1
	SIGX	t0

	#---- out-of-range conversions must saturate, not wrap. This is the
	#---- exact shape of a bug this project already fixed once.
	fcvt.wu.d t0, fa1		# negative -> unsigned
	SIGX	t0
	fcvt.w.d  t0, fa4		# +inf -> signed
	SIGX	t0
	fcvt.wu.d t0, fa5		# NaN -> unsigned
	SIGX	t0
	fcvt.l.d  t0, fa4
	SIGX	t0

	#---- conversions between the two widths: the instructions that belong
	#---- to both extensions at once
	fcvt.s.d ft3, fa1
	SIGS	ft3
	fcvt.d.s ft2, ft1
	SIGD	ft2
	fcvt.s.d ft3, fa5		# NaN across a width change
	SIGS	ft3

	#---- fused multiply-add, all four sign variants
	fmadd.d	 ft2, fa0, fa1, fa0
	SIGD	ft2
	fmsub.d	 ft2, fa0, fa1, fa0
	SIGD	ft2
	fnmadd.d ft2, fa0, fa1, fa0
	SIGD	ft2
	fnmsub.d ft2, fa0, fa1, fa0
	SIGD	ft2
	fmadd.s	 ft3, ft0, ft1, ft0
	SIGS	ft3

	#---- raw moves between the integer and FP register files
	fmv.x.d	t0, fa1
	SIGX	t0
	fmv.d.x	ft2, t0
	SIGD	ft2
	fmv.x.w	t0, ft1
	SIGX	t0

	#---- stores, read back through the integer side
	fsd	fa1, 64(s1)
	ld	t0, 64(s1)
	SIGX	t0
	fsw	ft1, 72(s1)
	lw	t0, 72(s1)
	SIGX	t0

	#---- accrued exception flags after all of the above
	frflags	t0
	SIGX	t0

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
fdata:
	.dword	0x3FF8000000000000	# 1.5
	.dword	0xC002000000000000	# -2.25
	.dword	0x0000000000000000	# +0.0
	.dword	0x8000000000000000	# -0.0
	.dword	0x7FF0000000000000	# +inf
	.dword	0x7FF8000000000000	# quiet NaN
	.word	0x3FC00000		# 1.5f
	.word	0xC0100000		# -2.25f
	.align 8
	.dword	0			# scratch for fsd  (offset 64)
	.dword	0			# scratch for fsw  (offset 72)

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
