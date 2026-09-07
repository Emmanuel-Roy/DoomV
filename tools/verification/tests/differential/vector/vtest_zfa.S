# Differential test for Zfa, the additional floating-point instructions.
#
# Zfa is unusual in this project because most of it is not new arithmetic --
# it is arithmetic that differs from an instruction already implemented only
# in a corner. That shapes what has to be tested:
#
#   * fminm/fmaxm agree with FMIN/FMAX on every input except a NaN operand,
#     where FMIN returns the other operand and fminm returns a canonical
#     NaN. Testing them on ordinary numbers proves nothing.
#
#   * fleq/fltq return exactly the same value as FLE/FLT for every input.
#     The only difference is which NaNs raise invalid: FLE/FLT raise it for
#     any NaN, fleq/fltq only for a signalling one. A test that dumps only
#     results cannot distinguish a correct implementation from one that
#     aliases them, so fflags is dumped after each comparison.
#
#   * fcvtmod.w.d wraps modulo 2^32 where every other float-to-int
#     conversion in RISC-V saturates. Its whole reason to exist is inputs
#     that do not fit, so the interesting cases are all out of range.
#
# fli is a 32-entry constant table. Every index is materialised and dumped
# as raw bits, because a table is exactly the kind of thing that is right in
# 30 places and wrong in two.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	# mstatus.FS must be on or every instruction here is illegal.
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
.macro SIGFLAGS				# accrued flags, then cleared
	frflags	t6
	SIGX	t6
	fsflags	x0
.endm

	#---- fli.d: all 32 table entries, dumped as bits
	fli.d	ft0, -1.0
	SIGD	ft0
	fli.d	ft0, min
	SIGD	ft0
	fli.d	ft0, 0x1p-16
	SIGD	ft0
	fli.d	ft0, 0x1p-15
	SIGD	ft0
	fli.d	ft0, 0x1p-8
	SIGD	ft0
	fli.d	ft0, 0x1p-7
	SIGD	ft0
	fli.d	ft0, 0x1p-4
	SIGD	ft0
	fli.d	ft0, 0x1p-3
	SIGD	ft0
	fli.d	ft0, 0.25
	SIGD	ft0
	fli.d	ft0, 0.3125
	SIGD	ft0
	fli.d	ft0, 0.375
	SIGD	ft0
	fli.d	ft0, 0.4375
	SIGD	ft0
	fli.d	ft0, 0.5
	SIGD	ft0
	fli.d	ft0, 0.625
	SIGD	ft0
	fli.d	ft0, 0.75
	SIGD	ft0
	fli.d	ft0, 0.875
	SIGD	ft0
	fli.d	ft0, 1.0
	SIGD	ft0
	fli.d	ft0, 1.25
	SIGD	ft0
	fli.d	ft0, 1.5
	SIGD	ft0
	fli.d	ft0, 1.75
	SIGD	ft0
	fli.d	ft0, 2.0
	SIGD	ft0
	fli.d	ft0, 2.5
	SIGD	ft0
	fli.d	ft0, 3.0
	SIGD	ft0
	fli.d	ft0, 4.0
	SIGD	ft0
	fli.d	ft0, 8.0
	SIGD	ft0
	fli.d	ft0, 16.0
	SIGD	ft0
	fli.d	ft0, 128.0
	SIGD	ft0
	fli.d	ft0, 256.0
	SIGD	ft0
	fli.d	ft0, 32768.0
	SIGD	ft0
	fli.d	ft0, 65536.0
	SIGD	ft0
	fli.d	ft0, inf
	SIGD	ft0
	fli.d	ft0, nan
	SIGD	ft0

	#---- fli.s: a sample across the table, including the two entries a
	#---- naive "convert the double table" implementation gets wrong --
	#---- min is 2^-126 here, not the double's 2^-1022.
	fli.s	ft1, -1.0
	SIGS	ft1
	fli.s	ft1, min
	SIGS	ft1
	fli.s	ft1, 0x1p-16
	SIGS	ft1
	fli.s	ft1, 0.3125
	SIGS	ft1
	fli.s	ft1, 1.0
	SIGS	ft1
	fli.s	ft1, 65536.0
	SIGS	ft1
	fli.s	ft1, inf
	SIGS	ft1
	fli.s	ft1, nan
	SIGS	ft1

	#---- load the operands the rest of the test uses
	fld	fa0, 0(s1)		# 1.5
	fld	fa1, 8(s1)		# -2.25
	fld	fa2, 16(s1)		# quiet NaN
	fld	fa3, 24(s1)		# signalling NaN
	fld	fa4, 32(s1)		# 2.5   (ties, for fround)
	fld	fa5, 40(s1)		# -2.5
	flw	ft2, 48(s1)		# 1.5f
	flw	ft3, 52(s1)		# -2.25f
	flw	ft4, 56(s1)		# quiet NaN (single)
	fsflags	x0

	#---- fminm/fmaxm on ordinary numbers: must agree with FMIN/FMAX
	fminm.d	ft0, fa0, fa1
	SIGD	ft0
	fmaxm.d	ft0, fa0, fa1
	SIGD	ft0
	fminm.s	ft1, ft2, ft3
	SIGS	ft1
	fmaxm.s	ft1, ft2, ft3
	SIGS	ft1
	SIGFLAGS

	#---- and on a NaN operand, which is the whole point: FMIN would return
	#---- the *other* operand, fminm must return a canonical NaN
	fminm.d	ft0, fa0, fa2		# quiet NaN -> canonical NaN, no NV
	SIGD	ft0
	SIGFLAGS
	fmaxm.d	ft0, fa2, fa0
	SIGD	ft0
	SIGFLAGS
	fminm.d	ft0, fa0, fa3		# signalling NaN -> canonical NaN + NV
	SIGD	ft0
	SIGFLAGS
	fminm.s	ft1, ft2, ft4
	SIGS	ft1
	SIGFLAGS

	#---- fmin.d for contrast: the same NaN input, different answer. If
	#---- fminm were implemented as fmin these two would agree, and they
	#---- must not.
	fmin.d	ft0, fa0, fa2
	SIGD	ft0
	SIGFLAGS

	#---- fround/froundnx. The distinction is only the inexact flag, so
	#---- each is followed by its flags.
	fround.d ft0, fa0		# 1.5 -> 2.0 (RNE, ties to even)
	SIGD	ft0
	SIGFLAGS
	froundnx.d ft0, fa0		# same value, but NX set
	SIGD	ft0
	SIGFLAGS
	fround.d ft0, fa4		# 2.5 -> 2.0 under RNE
	SIGD	ft0
	SIGFLAGS
	fround.d ft0, fa5		# -2.5 -> -2.0
	SIGD	ft0
	SIGFLAGS
	fround.d ft0, fa1		# -2.25 -> -2.0
	SIGD	ft0
	SIGFLAGS
	froundnx.d ft0, fa1
	SIGD	ft0
	SIGFLAGS
	fround.s ft1, ft2
	SIGS	ft1
	froundnx.s ft1, ft3
	SIGS	ft1
	SIGFLAGS

	#---- an already-integral value must not set NX even for froundnx
	fli.d	ft5, 4.0
	froundnx.d ft0, ft5
	SIGD	ft0
	SIGFLAGS

	#---- fround with explicit rounding modes rather than dynamic
	fround.d ft0, fa0, rtz
	SIGD	ft0
	fround.d ft0, fa0, rdn
	SIGD	ft0
	fround.d ft0, fa0, rup
	SIGD	ft0
	fround.d ft0, fa1, rtz
	SIGD	ft0
	fround.d ft0, fa1, rup
	SIGD	ft0
	SIGFLAGS

	#---- fcvtmod.w.d: wraps instead of saturating, so every interesting
	#---- case is out of int32 range.
	fld	ft6, 64(s1)		# 2^32 + 5      -> 5
	fcvtmod.w.d t0, ft6, rtz
	SIGX	t0
	SIGFLAGS
	fld	ft6, 72(s1)		# 2^31          -> -2^31
	fcvtmod.w.d t0, ft6, rtz
	SIGX	t0
	SIGFLAGS
	fld	ft6, 80(s1)		# -(2^32) - 3   -> -3
	fcvtmod.w.d t0, ft6, rtz
	SIGX	t0
	SIGFLAGS
	fld	ft6, 88(s1)		# 1e300, far past any integer width
	fcvtmod.w.d t0, ft6, rtz
	SIGX	t0
	SIGFLAGS
	fcvtmod.w.d t0, fa0, rtz	# 1.5 -> 1, inexact
	SIGX	t0
	SIGFLAGS
	fcvtmod.w.d t0, fa1, rtz	# -2.25 -> -2
	SIGX	t0
	SIGFLAGS
	fcvtmod.w.d t0, fa2, rtz	# NaN -> 0, NV
	SIGX	t0
	SIGFLAGS
	fld	ft6, 96(s1)		# +inf -> 0, NV
	fcvtmod.w.d t0, ft6, rtz
	SIGX	t0
	SIGFLAGS

	#---- out of range *and* non-integral, which exercises whether NV and
	#---- NX combine or one suppresses the other
	fld	ft6, 104(s1)
	fcvtmod.w.d t0, ft6, rtz
	SIGX	t0
	SIGFLAGS

	#---- fcvt.w.d for contrast: the same out-of-range input saturates
	fld	ft6, 72(s1)
	fcvt.w.d t0, ft6, rtz
	SIGX	t0
	SIGFLAGS

	#---- fleq/fltq: same results as fle/flt, different flags on NaN
	fleq.d	t0, fa0, fa1
	SIGX	t0
	fltq.d	t0, fa0, fa1
	SIGX	t0
	fleq.d	t0, fa1, fa0
	SIGX	t0
	fltq.d	t0, fa1, fa0
	SIGX	t0
	fleq.d	t0, fa0, fa0
	SIGX	t0
	SIGFLAGS

	#---- a quiet NaN: result 0 and, crucially, NO invalid flag
	fleq.d	t0, fa0, fa2
	SIGX	t0
	SIGFLAGS
	fltq.d	t0, fa2, fa0
	SIGX	t0
	SIGFLAGS

	#---- fle.d with the same quiet NaN: result also 0, but NV *is* set.
	#---- This pair is what proves the two are not aliases.
	fle.d	t0, fa0, fa2
	SIGX	t0
	SIGFLAGS

	#---- a signalling NaN raises invalid for the quiet forms too
	fleq.d	t0, fa0, fa3
	SIGX	t0
	SIGFLAGS

	#---- and the single-precision quiet forms
	fleq.s	t0, ft2, ft3
	SIGX	t0
	fltq.s	t0, ft2, ft4
	SIGX	t0
	SIGFLAGS

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
	.dword	0x3FF8000000000000	# 1.5              (0)
	.dword	0xC002000000000000	# -2.25             (8)
	.dword	0x7FF8000000000000	# quiet NaN        (16)
	.dword	0x7FF4000000000000	# signalling NaN   (24)
	.dword	0x4004000000000000	# 2.5              (32)
	.dword	0xC004000000000000	# -2.5             (40)
	.word	0x3FC00000		# 1.5f             (48)
	.word	0xC0100000		# -2.25f           (52)
	.word	0x7FC00000		# quiet NaN single (56)
	.word	0			# padding          (60)
	.dword	0x41F0000000500000	# 2^32 + 5         (64)
	.dword	0x41E0000000000000	# 2^31             (72)
	.dword	0xC1F0000000300000	# -(2^32) - 3      (80)
	.dword	0x7E37E43C8800759C	# 1e300            (88)
	.dword	0x7FF0000000000000	# +inf             (96)
	.dword	0x41F0000000000280	# 2^32 + 0.00061  (104) -- out of range AND
					#                       non-integral, so both
					#                       NV and NX are in play

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
