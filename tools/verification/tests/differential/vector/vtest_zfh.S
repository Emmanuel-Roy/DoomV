# Differential test for Zfhmin, minimal half-precision floating point.
#
# RVA23U64 mandates Zfhmin, not full Zfh -- half-precision *arithmetic* is an
# expansion option. So there is no fadd.h to test here. What there is:
# load/store, bit moves, and conversions, and the conversions are where all
# the risk lives.
#
# MinGW's GCC 8.1 has no _Float16 on x86, so DoomV cannot delegate any of
# this to the host FPU; every conversion is hand-written integer
# manipulation. That makes this suite less a check on plumbing than on
# arithmetic, and the cases below are chosen accordingly:
#
#   * Subnormal halves. Half's exponent range is tiny, so values that are
#     perfectly ordinary in single precision land in half's subnormal range
#     or below it. Widening a half subnormal requires renormalising it;
#     narrowing into one requires shifting the significand down and rounding
#     what falls off.
#
#   * Exact midpoints. Ties-to-even is only observable when the discarded
#     part is exactly half an ulp, which needs deliberately constructed
#     inputs -- an ordinary value never lands there.
#
#   * Overflow, which is not always infinity. Under round-toward-zero, and
#     under rounding away from the value's own sign, it produces the largest
#     finite half instead. Every rounding mode is exercised on the same
#     overflowing input.
#
#   * Double-rounding. fcvt.h.d must round once, directly from the double's
#     52 fraction bits. An implementation that goes double -> float -> half
#     rounds twice and disagrees at values that are exact midpoints in half
#     but not in single; one such value is included.
#
# Every result is dumped as raw bits, and fflags after each conversion,
# because underflow and inexact are the whole difference between several of
# these cases.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	li	t0, 1 << 13		# mstatus.FS
	csrs	mstatus, t0

	la	s0, begin_signature
	la	s1, hdata
	la	s2, ddata

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm
.macro SIGH freg			# half, as raw bits via the integer side
	fmv.x.h	t6, \freg
	SIGX	t6
.endm
.macro SIGS freg
	fmv.x.w	t6, \freg
	SIGX	t6
.endm
.macro SIGD freg
	fmv.x.d	t6, \freg
	SIGX	t6
.endm
.macro SIGFLAGS
	frflags	t6
	SIGX	t6
	fsflags	x0
.endm

	#---- flh / fmv.x.h: load each test half and read its bits back. If
	#---- flh failed to NaN-box, fmv.x.h would return the canonical NaN
	#---- instead of the value, so this pair checks boxing as well.
	flh	ft0, 0(s1)		# 1.0
	SIGH	ft0
	flh	ft0, 2(s1)		# -2.5
	SIGH	ft0
	flh	ft0, 4(s1)		# smallest subnormal
	SIGH	ft0
	flh	ft0, 6(s1)		# largest subnormal
	SIGH	ft0
	flh	ft0, 8(s1)		# smallest normal
	SIGH	ft0
	flh	ft0, 10(s1)		# largest finite
	SIGH	ft0
	flh	ft0, 12(s1)		# +inf
	SIGH	ft0
	flh	ft0, 14(s1)		# quiet NaN
	SIGH	ft0
	flh	ft0, 16(s1)		# -0.0
	SIGH	ft0
	SIGFLAGS

	#---- fmv.h.x / fsh: bits in through the integer side, stored out, and
	#---- read back as an integer to confirm the two bytes that landed
	li	t1, 0xC500		# -5.0 in binary16
	fmv.h.x	ft1, t1
	SIGH	ft1
	la	t2, scratch
	fsh	ft1, 0(t2)
	lhu	t3, 0(t2)
	SIGX	t3
	# fmv.h.x takes the low 16 bits and ignores the rest
	li	t1, 0xFFFFFFFFFFFF3C00
	fmv.h.x	ft1, t1
	SIGH	ft1
	SIGFLAGS

	#---- fcvt.s.h: widening is exact for every input, including the
	#---- subnormals, which have to be renormalised on the way up
	flh	ft0, 0(s1)
	fcvt.s.h ft2, ft0
	SIGS	ft2
	flh	ft0, 2(s1)
	fcvt.s.h ft2, ft0
	SIGS	ft2
	flh	ft0, 4(s1)		# smallest subnormal -> a normal single
	fcvt.s.h ft2, ft0
	SIGS	ft2
	flh	ft0, 6(s1)		# largest subnormal
	fcvt.s.h ft2, ft0
	SIGS	ft2
	flh	ft0, 12(s1)		# +inf
	fcvt.s.h ft2, ft0
	SIGS	ft2
	flh	ft0, 14(s1)		# NaN -> canonical single NaN
	fcvt.s.h ft2, ft0
	SIGS	ft2
	flh	ft0, 16(s1)		# -0.0 keeps its sign
	fcvt.s.h ft2, ft0
	SIGS	ft2
	SIGFLAGS

	#---- fcvt.d.h: the same, into double
	flh	ft0, 4(s1)
	fcvt.d.h ft3, ft0
	SIGD	ft3
	flh	ft0, 10(s1)
	fcvt.d.h ft3, ft0
	SIGD	ft3
	flh	ft0, 14(s1)
	fcvt.d.h ft3, ft0
	SIGD	ft3
	SIGFLAGS

	#---- fcvt.h.s: narrowing, where the rounding happens.
	flw	ft2, 0(s2)		# 1.0f -> exact
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 4(s2)		# 1.0f + 1 half-ulp: an exact midpoint,
	fcvt.h.s ft0, ft2		# ties-to-even must pick the even one
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 8(s2)		# just above that midpoint -> rounds up
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 12(s2)		# 65520.0f: rounds up to +inf under RNE
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 16(s2)		# 1e30f: overflow
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 20(s2)		# 1e-8f: underflows to zero, UF + NX
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 24(s2)		# a value landing in half's subnormal range
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 28(s2)		# single NaN -> canonical half NaN
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 32(s2)		# -inf
	fcvt.h.s ft0, ft2
	SIGH	ft0
	SIGFLAGS

	#---- overflow under every rounding mode. RNE and RUP give +inf for a
	#---- positive value; RTZ and RDN give the largest finite half. Getting
	#---- this wrong is invisible unless the modes are tested separately.
	flw	ft2, 16(s2)		# +1e30f
	fcvt.h.s ft0, ft2, rne
	SIGH	ft0
	fcvt.h.s ft0, ft2, rtz
	SIGH	ft0
	fcvt.h.s ft0, ft2, rdn
	SIGH	ft0
	fcvt.h.s ft0, ft2, rup
	SIGH	ft0
	SIGFLAGS
	flw	ft2, 36(s2)		# -1e30f: the signs swap which mode saturates
	fcvt.h.s ft0, ft2, rne
	SIGH	ft0
	fcvt.h.s ft0, ft2, rtz
	SIGH	ft0
	fcvt.h.s ft0, ft2, rdn
	SIGH	ft0
	fcvt.h.s ft0, ft2, rup
	SIGH	ft0
	SIGFLAGS

	#---- a midpoint under each rounding mode
	flw	ft2, 4(s2)
	fcvt.h.s ft0, ft2, rne
	SIGH	ft0
	fcvt.h.s ft0, ft2, rtz
	SIGH	ft0
	fcvt.h.s ft0, ft2, rdn
	SIGH	ft0
	fcvt.h.s ft0, ft2, rup
	SIGH	ft0
	SIGFLAGS

	#---- fcvt.h.d, including the double-rounding trap: a value that is an
	#---- exact midpoint in half but NOT in single, so converting via
	#---- single first perturbs it and rounds the other way.
	fld	ft3, 48(s2)		# 1.0
	fcvt.h.d ft0, ft3
	SIGH	ft0
	SIGFLAGS
	fld	ft3, 56(s2)		# exact half-midpoint, tie to even
	fcvt.h.d ft0, ft3
	SIGH	ft0
	SIGFLAGS
	fld	ft3, 64(s2)		# midpoint + 1 double-ulp: must round up,
	fcvt.h.d ft0, ft3		# and would not if it went via single
	SIGH	ft0
	SIGFLAGS
	fld	ft3, 72(s2)		# subnormal-range double
	fcvt.h.d ft0, ft3
	SIGH	ft0
	SIGFLAGS
	fld	ft3, 80(s2)		# 1e300 -> overflow
	fcvt.h.d ft0, ft3
	SIGH	ft0
	SIGFLAGS

	#---- round trip: half -> single -> half must be the identity for every
	#---- representable half, subnormals included
	flh	ft0, 4(s1)
	fcvt.s.h ft2, ft0
	fcvt.h.s ft1, ft2
	SIGH	ft1
	flh	ft0, 6(s1)
	fcvt.s.h ft2, ft0
	fcvt.h.s ft1, ft2
	SIGH	ft1
	flh	ft0, 10(s1)
	fcvt.s.h ft2, ft0
	fcvt.h.s ft1, ft2
	SIGH	ft1
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
hdata:
	.half	0x3C00			# 1.0                    (0)
	.half	0xC100			# -2.5                   (2)
	.half	0x0001			# smallest subnormal     (4)
	.half	0x03FF			# largest subnormal      (6)
	.half	0x0400			# smallest normal        (8)
	.half	0x7BFF			# largest finite         (10)
	.half	0x7C00			# +inf                   (12)
	.half	0x7E00			# quiet NaN              (14)
	.half	0x8000			# -0.0                   (16)
	.half	0			# padding                (18)

	.align 8
ddata:
	.word	0x3F800000		# 1.0f                   (0)
	.word	0x3F801000		# 1.0f + half an ulp of half precision  (4)
	.word	0x3F801001		# just above that midpoint              (8)
	.word	0x477FF000		# 65520.0f -> +inf under RNE           (12)
	.word	0x7149F2CA		# 1e30f                                (16)
	.word	0x322BCC77		# 1e-8f                                (20)
	.word	0x33800000		# 2^-24, half's smallest subnormal     (24)
	.word	0x7FC00000		# single quiet NaN                     (28)
	.word	0xFF800000		# -inf                                 (32)
	.word	0xF149F2CA		# -1e30f                               (36)
	.word	0, 0			# padding                              (40)
	.dword	0x3FF0000000000000	# 1.0                          (48)
	.dword	0x3FF0020000000000	# exact half-midpoint          (56)
	.dword	0x3FF0020000000001	# midpoint + 1 double-ulp      (64)
	.dword	0x3E70000000000000	# 2^-24                        (72)
	.dword	0x7E37E43C8800759C	# 1e300                        (80)

	.align 8
scratch:
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
