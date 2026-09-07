# Differential test for the vector float conversion family: Zvfhmin's
# vfwcvt.f.f.v / vfncvt.f.f.w at SEW 16<->32, and the rest of VFUNARY0's
# widening and narrowing subcodes.
#
# All of these were previously no-ops. The VFUNARY0 handler accepted only
# the same-width conversions and returned for everything else, so a guest
# executing vfwcvt.f.f.v got its destination register left at whatever it
# held before -- no trap, no diagnostic. That is the same failure Zvbb had,
# and it is why this suite covers the whole family rather than only the two
# instructions RVA23 mandates: implementing a subset and leaving the rest
# silently ignored is what created the problem.
#
# Half precision is the interesting width. DoomV has no 16-bit float type
# available -- MinGW's GCC 8.1 has no _Float16 on x86 -- so every half
# conversion here runs through hand-written integer code, including
# subnormals and rounding. The narrow side is also where a two-step
# implementation would double-round, which vfncvt.rod.f.f.w exists
# specifically to prevent.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	li	t0, (1 << 13) | (1 << 9)	# mstatus FS + VS
	csrs	mstatus, t0

	la	s0, begin_signature
	la	s1, hvals
	la	s2, fvals
	la	s3, dvals
	la	s4, ivals

.macro SIGV vreg
	csrr	t4, vtype
	csrr	t5, vl
	vsetvli	t6, x0, e8, m1, ta, ma
	vse8.v	\vreg, (s0)
	addi	s0, s0, 16
	vsetvl	t6, t5, t4
.endm
.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm
.macro SIGFLAGS
	frflags	t6
	SIGX	t6
	fsflags	x0
.endm

	#---- vfwcvt.f.f.v, SEW 16 -> 32. This is the Zvfhmin one. The source
	#---- halves include subnormals, which have to be renormalised, and a
	#---- NaN, which must come out canonical.
	vsetvli	t0, x0, e16, m1, ta, ma
	vle16.v	v1, (s1)
	vfwcvt.f.f.v v8, v1
	SIGV	v8
	SIGV	v9
	SIGFLAGS

	#---- vfncvt.f.f.w, SEW 32 -> 16: the rounding direction. The source
	#---- singles are built to land on midpoints, in half's subnormal
	#---- range, and past its maximum.
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v8, (s2)
	addi	t3, s2, 16
	vle32.v	v9, (t3)
	vsetvli	t0, x0, e16, m1, ta, ma
	vfncvt.f.f.w v2, v8
	SIGV	v2
	SIGFLAGS

	#---- vfncvt.rod.f.f.w is deliberately NOT tested at half width here.
	#---- Zvfhmin permits exactly two half-precision vector instructions,
	#---- vfwcvt.f.f.v and vfncvt.f.f.w; the round-to-odd form is not one of
	#---- them, and spike correctly refuses it under -march=...zvfhmin. It
	#---- is exercised at 64->32 below, where it is plain base V.

	#---- the same pair at the widths base V has always nominally
	#---- supported and never actually implemented: 32 <-> 64
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v1, (s2)
	vfwcvt.f.f.v v8, v1
	SIGV	v8
	SIGV	v9
	SIGFLAGS
	vsetvli	t0, x0, e64, m1, ta, ma
	vle64.v	v8, (s3)
	addi	t3, s3, 16
	vle64.v	v9, (t3)
	vsetvli	t0, x0, e32, m1, ta, ma
	vfncvt.f.f.w v2, v8
	SIGV	v2
	SIGFLAGS
	vfncvt.rod.f.f.w v3, v8
	SIGV	v3
	SIGFLAGS

	#---- integer/float widening converts
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v1, (s4)		# integer source
	vfwcvt.f.x.v v8, v1		# signed int32 -> double
	SIGV	v8
	SIGV	v9
	SIGFLAGS
	vfwcvt.f.xu.v v8, v1		# unsigned int32 -> double
	SIGV	v8
	SIGV	v9
	SIGFLAGS
	vle32.v	v1, (s2)		# float source
	vfwcvt.x.f.v v8, v1		# float -> signed int64
	SIGV	v8
	SIGV	v9
	SIGFLAGS
	vfwcvt.rtz.x.f.v v8, v1		# ...truncating, ignoring frm
	SIGV	v8
	SIGV	v9
	SIGFLAGS
	vfwcvt.rtz.xu.f.v v8, v1
	SIGV	v8
	SIGV	v9
	SIGFLAGS

	#---- and the narrowing integer/float converts
	vsetvli	t0, x0, e64, m1, ta, ma
	vle64.v	v8, (s3)
	addi	t3, s3, 16
	vle64.v	v9, (t3)
	vsetvli	t0, x0, e32, m1, ta, ma
	vfncvt.x.f.w v2, v8		# double -> signed int32
	SIGV	v2
	SIGFLAGS
	vfncvt.rtz.x.f.w v2, v8
	SIGV	v2
	SIGFLAGS
	vfncvt.rtz.xu.f.w v2, v8
	SIGV	v2
	SIGFLAGS
	vsetvli	t0, x0, e64, m1, ta, ma
	vle64.v	v8, (s4)		# integer source, 64-bit
	addi	t3, s4, 16
	vle64.v	v9, (t3)
	vsetvli	t0, x0, e32, m1, ta, ma
	vfncvt.f.x.w v2, v8		# signed int64 -> float
	SIGV	v2
	SIGFLAGS
	vfncvt.f.xu.w v2, v8
	SIGV	v2
	SIGFLAGS

	#---- masked: the convert path must honour v0 like every other vector
	#---- operation. Destination pre-set so inactive elements are visible.
	vsetvli	t0, x0, e16, m1, ta, ma
	la	t2, maskv
	vle16.v	v0, (t2)
	vle16.v	v1, (s1)
	vsetvli	t0, x0, e32, m1, ta, ma
	vmv.v.i	v8, -1
	vmv.v.i	v9, -1
	vsetvli	t0, x0, e16, m1, ta, ma
	vfwcvt.f.f.v v8, v1, v0.t
	SIGV	v8
	SIGV	v9
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
hvals:					# eight binary16 values
	.half	0x3C00			# 1.0
	.half	0xC100			# -2.5
	.half	0x0001			# smallest subnormal
	.half	0x03FF			# largest subnormal
	.half	0x0400			# smallest normal
	.half	0x7BFF			# largest finite
	.half	0x7E00			# quiet NaN
	.half	0x8000			# -0.0

	.align 8
fvals:					# eight binary32 values
	.word	0x3F800000		# 1.0
	.word	0x3F801000		# an exact midpoint in half precision
	.word	0x3F801001		# just above it
	.word	0x7149F2CA		# 1e30 -> overflows half
	.word	0x33800000		# 2^-24, half's smallest subnormal
	.word	0x322BCC77		# 1e-8 -> underflows half
	.word	0xC0200000		# -2.5
	.word	0x7FC00000		# quiet NaN

	.align 8
dvals:					# four binary64 values
	.dword	0x3FF0000000000000	# 1.0
	.dword	0x3FF0000010000000	# an exact midpoint in single precision
	.dword	0xC004000000000000	# -2.5
	.dword	0x7FF8000000000000	# quiet NaN

	.align 8
ivals:
	.word	1, -2, 100000, -100000
	.word	0, 0, 0, 0

	.align 8
maskv:
	.dword	0x55			# alternate elements active
	.dword	0

	.align 4
	.global begin_signature
begin_signature:
	.fill	2048, 1, 0
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
