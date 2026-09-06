# Differential test for Zvbb, the vector bit-manipulation extension.
#
# Until now DoomV implemented exactly one Zvbb instruction, vandn, and the
# rest of the extension was not merely missing -- it was silently ignored.
# An unimplemented funct6 in the OPIV* space fell through to exec_v_int,
# which did nothing and advanced pc, so a guest using vrol or vwsll got a
# destination register left at whatever it held before. That is the same
# shape as every other bug this harness has caught: a wrong answer with no
# crash.
#
# The cases below are chosen around the two things most likely to be wrong,
# both of which are element-width dependent:
#
#   * clz/ctz/cpop are defined over SEW, not 64 bits. An all-zero element
#     must yield SEW, and a host __builtin_clzll would answer 64 for every
#     width. So each is run at e8 and e32, including the zero element.
#
#   * rotates are modulo SEW and must happen inside it. A 64-bit host rotate
#     of an 8-bit element drags in the zeros above bit 7, which looks right
#     at e64 and is wrong everywhere else. So rotates are run at e8 as well.
#
# vror.vi additionally needs a 6-bit rotate amount out of a 5-bit immediate
# field, with imm[5] stored in the low bit of funct6 -- so an amount above
# 31 exercises a different encoding path than one below, and both are here.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	# Vector state is off at reset and DoomV enforces that now, so this
	# must come before any vector instruction or they are all illegal.
	li	t0, (1 << 13) | (1 << 9)	# FS=Initial, VS=Initial
	csrs	mstatus, t0

	la	s0, begin_signature
	la	s1, vdata
	la	s2, vdata2

# Dump a whole vector register as bytes, restoring vtype/vl afterwards so
# the dump cannot perturb the sequence being tested.
.macro SIGV vreg
	csrr	t4, vtype
	csrr	t5, vl
	vsetvli	t6, x0, e8, m1, ta, ma
	vse8.v	\vreg, (s0)
	addi	s0, s0, 16
	vsetvl	t6, t5, t4
.endm

	#---- vandn: the one instruction this extension already had, kept here
	#---- so the new file covers the whole extension rather than only the
	#---- additions.
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v1, (s1)
	vle32.v	v2, (s2)
	vandn.vv v5, v1, v2
	SIGV	v5
	li	t1, 0x0f0f0f0f
	vandn.vx v5, v1, t1
	SIGV	v5

	#---- byte and bit reversal at e32
	vbrev.v	v5, v1
	SIGV	v5
	vbrev8.v v5, v1
	SIGV	v5
	vrev8.v	v5, v1
	SIGV	v5

	#---- the same three at e8, where "within a byte" and "across the
	#---- element" stop being distinguishable unless the width is honoured
	vsetvli	t0, x0, e8, m1, ta, ma
	vle8.v	v1, (s1)
	vbrev.v	v5, v1
	SIGV	v5
	vbrev8.v v5, v1
	SIGV	v5
	vrev8.v	v5, v1
	SIGV	v5

	#---- bit counting at e8. vdata's first bytes include 0x00, so the
	#---- all-zero element case (result must be SEW, i.e. 8) is covered.
	vclz.v	v5, v1
	SIGV	v5
	vctz.v	v5, v1
	SIGV	v5
	vcpop.v	v5, v1
	SIGV	v5

	#---- and at e32, where the zero-element answer must become 32
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v1, (s1)
	la	t2, s3zero
	vle32.v	v3, (t2)
	vclz.v	v5, v1
	SIGV	v5
	vclz.v	v5, v3			# every element zero -> 32
	SIGV	v5
	vctz.v	v5, v1
	SIGV	v5
	vctz.v	v5, v3			# -> 32
	SIGV	v5
	vcpop.v	v5, v1
	SIGV	v5

	#---- rotates at e32
	vrol.vv	v5, v1, v2
	SIGV	v5
	li	t1, 5
	vrol.vx	v5, v1, t1
	SIGV	v5
	vror.vv	v5, v1, v2
	SIGV	v5
	vror.vx	v5, v1, t1
	SIGV	v5
	vror.vi	v5, v1, 7		# imm[5] == 0 -> funct6 0x14
	SIGV	v5
	vror.vi	v5, v1, 31
	SIGV	v5

	#---- rotates at e8, where a 64-bit host rotate gives a wrong answer
	vsetvli	t0, x0, e8, m1, ta, ma
	vle8.v	v1, (s1)
	vle8.v	v2, (s2)
	vrol.vv	v5, v1, v2
	SIGV	v5
	li	t1, 3
	vrol.vx	v5, v1, t1
	SIGV	v5
	vror.vv	v5, v1, v2
	SIGV	v5
	vror.vx	v5, v1, t1
	SIGV	v5
	li	t1, 11			# > 8: the amount is taken modulo SEW
	vrol.vx	v5, v1, t1
	SIGV	v5

	#---- vror.vi with an amount above 31, which encodes imm[5] in the low
	#---- bit of funct6 and so takes a different decode path entirely
	vsetvli	t0, x0, e64, m1, ta, ma
	vle64.v	v1, (s1)
	vror.vi	v5, v1, 40
	SIGV	v5
	vror.vi	v5, v1, 63
	SIGV	v5

	#---- vwsll: widening, so the destination is a 2-register group at
	#---- 2*SEW. Both halves are dumped -- an implementation that wrote the
	#---- narrow width would leave the second register untouched.
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v1, (s1)
	vle32.v	v2, (s2)
	vwsll.vv v8, v1, v2
	SIGV	v8
	SIGV	v9
	li	t1, 6
	vwsll.vx v8, v1, t1
	SIGV	v8
	SIGV	v9
	vwsll.vi v8, v1, 5
	SIGV	v8
	SIGV	v9

	#---- masked operation: vandn under a mask, to confirm the Zvbb path
	#---- honours v0 and tail/inactive policy like every other vector op.
	#---- This is the class of bug that already bit this project once, when
	#---- set_mask_bit wrote v0 instead of vd.
	vsetvli	t0, x0, e32, m1, ta, ma
	la	t2, s4mask
	vle32.v	v0, (t2)
	vmv.v.i	v5, 0
	vandn.vv v5, v1, v2, v0.t
	SIGV	v5
	vmv.v.i	v5, 0
	vrol.vv	v5, v1, v2, v0.t
	SIGV	v5

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
vdata:
	.dword	0x0123456789abcdef
	.dword	0xfedcba9800ff0001
vdata2:
	.dword	0x0000000300000005	# small rotate amounts at e32
	.dword	0x0000000700000001
s3zero:
	.dword	0
	.dword	0
s4mask:
	.dword	0x5			# elements 0 and 2 active at e32
	.dword	0

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
