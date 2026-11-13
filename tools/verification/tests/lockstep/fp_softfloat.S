# The floating-point paths that did not go through SoftFloat, lock-stepped
# against Sail on the inputs where they were wrong (BUGS 193-195).
#
#   1. Scalar FCVT rounded RMM (ties away) as RNE (ties to even): the host
#      has no such mode. Exact ties, in every direction and width; Zfa's
#      fround/froundnx and Zfhmin's fcvt.h.s/fcvt.h.d had the same flaw.
#   2. FMIN/FMAX with one signaling NaN took the host C library's answer,
#      which on glibc is a quieted NaN rather than the other operand.
#   3. Vector SEW=32 narrowed each element through a host float, which quiets
#      a signaling NaN before SoftFloat sees it, so NV was never raised.
#
# After each operation fflags is read into an integer register and cleared
# (csrrw rd, fflags, zero), so the flags are compared as register writes,
# operation by operation, rather than accrued.
#
# M-mode throughout; nothing here traps. tohost and fromhost carry their
# sizes because QEMU's spike machine (corun.py) will not load without them.

.option arch, +f, +d, +v, +zfa, +zfh

.equ MSTATUS_FS, (1 << 13)
.equ MSTATUS_VS, (1 << 9)

.equ SNAN_S,  0x7FA49854         # single, signaling (quiet bit clear)
.equ SNAN_S1, 0x7F800001         # single, signaling, smallest payload
.equ ONE_S,   0x3F800000         # 1.0f

# Read and clear the flags into rd.
.macro FLAGS rd
	csrrw \rd, fflags, zero
.endm

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, MSTATUS_FS | MSTATUS_VS
	csrs mstatus, t0
	csrwi frm, 0
	csrwi fflags, 0

	# ---- 1. scalar FCVT, RMM ----------------------------------------------------
	li   t0, 0x40200000              # 2.5f
	fmv.w.x f1, t0
	fcvt.w.s  a0, f1, rmm            # 3, NX
	FLAGS a1
	fcvt.l.s  a0, f1, rmm            # 3, NX
	FLAGS a1
	fcvt.lu.s a0, f1, rmm            # 3, NX
	FLAGS a1
	fcvt.w.s  a0, f1, rne            # 2, NX: the tie RNE breaks the other way
	FLAGS a1

	li   t0, 0xBF000000              # -0.5f
	fmv.w.x f2, t0
	fcvt.wu.s a0, f2, rmm            # rounds to -1: 0, NV
	FLAGS a1
	fcvt.w.s  a0, f2, rmm            # -1, NX
	FLAGS a1
	fcvt.wu.s a0, f2, rne            # rounds to -0: 0, NX
	FLAGS a1

	li   t0, 0x1000001               # 2^24 + 1: a tie at single precision
	fcvt.s.w  f3, t0, rmm            # 0x4B800001
	FLAGS a1
	fmv.x.w a0, f3
	fcvt.s.wu f3, t0, rmm
	FLAGS a1
	fmv.x.w a0, f3
	fcvt.s.l  f3, t0, rmm
	FLAGS a1
	fmv.x.w a0, f3
	fcvt.s.w  f3, t0, rne            # 0x4B800000
	FLAGS a1
	fmv.x.w a0, f3

	li   t0, 0x20000000000001        # 2^53 + 1: a tie at double precision
	fcvt.d.l  f4, t0, rmm            # 0x4340000000000001
	FLAGS a1
	fmv.x.d a0, f4
	fcvt.d.lu f4, t0, rmm
	FLAGS a1
	fmv.x.d a0, f4
	fcvt.s.lu f3, t0, rmm            # single precision too
	FLAGS a1
	fmv.x.w a0, f3

	li   t0, 0x3FF0000010000000      # 1 + 2^-24: halfway between two singles
	fmv.d.x f5, t0
	fcvt.s.d  f3, f5, rmm            # 0x3F800001
	FLAGS a1
	fmv.x.w a0, f3
	fcvt.s.d  f3, f5, rne            # 0x3F800000
	FLAGS a1
	fmv.x.w a0, f3

	li   t0, 0xC1E0000000100000      # -2^31 - 0.5
	fmv.d.x f6, t0
	fcvt.w.d  a0, f6, rmm            # rounds to -2^31 - 1: INT32_MIN, NV
	FLAGS a1
	fcvt.w.d  a0, f6, rne            # rounds to -2^31: in range, NX
	FLAGS a1
	li   t0, 0x4004000000000000      # 2.5
	fmv.d.x f7, t0
	fcvt.l.d  a0, f7, rmm            # 3
	FLAGS a1
	fcvt.wu.d a0, f7, rmm            # 3
	FLAGS a1
	fcvt.lu.d a0, f7, rmm            # 3
	FLAGS a1

	# The dynamic encoding reaches the same mode through frm.
	csrwi frm, 4
	fcvt.w.s  a0, f1                 # 3
	FLAGS a1
	fcvt.s.d  f3, f5                 # 0x3F800001
	FLAGS a1
	fmv.x.w a0, f3
	csrwi frm, 0

	# Zfa fround/froundnx and Zfhmin fcvt.h.*, the same ties.
	fround.s   f3, f1, rmm            # 3.0
	FLAGS a1
	fmv.x.w a0, f3
	froundnx.s f3, f1, rmm            # 3.0, NX
	FLAGS a1
	fmv.x.w a0, f3
	fround.s   f3, f1, rne            # 2.0
	FLAGS a1
	fmv.x.w a0, f3
	froundnx.d f4, f7, rmm            # 3.0, NX
	FLAGS a1
	fmv.x.d a0, f4
	li   t0, SNAN_S
	fmv.w.x f20, t0
	fround.s   f3, f20, rmm           # canonical NaN, NV
	FLAGS a1
	fmv.x.w a0, f3
	li   t0, 0x3F801000              # 1 + 2^-11: halfway between two halves
	fmv.w.x f21, t0
	fcvt.h.s   f3, f21, rmm           # 0x3C01
	FLAGS a1
	fmv.x.w a0, f3
	fcvt.h.s   f3, f21, rne           # 0x3C00
	FLAGS a1
	fmv.x.w a0, f3
	li   t0, 0x3FF0020000000000      # the same tie as a double
	fmv.d.x f22, t0
	fcvt.h.d   f3, f22, rmm           # 0x3C01
	FLAGS a1
	fmv.x.w a0, f3

	# Zfa, rebuilt on SoftFloat with the rest of the unit: fcvtmod.w.d (truncate,
	# wrap modulo 2^32), fminm/fmaxm (any NaN gives NaN), fleq/fltq (quiet).
	li   t0, 0x41F0000000400000      # 2^32 + 4: wraps to 4, NV
	fmv.d.x f23, t0
	fcvtmod.w.d a0, f23, rtz
	FLAGS a1
	li   t0, 0x4544000000000000      # 2^85: wraps to 0, NV
	fmv.d.x f23, t0
	fcvtmod.w.d a0, f23, rtz
	FLAGS a1
	li   t0, 0xC1E0000000100000      # -2^31 - 0.5: truncates to -2^31, in range, NX
	fmv.d.x f23, t0
	fcvtmod.w.d a0, f23, rtz
	FLAGS a1
	li   t0, 0xC004000000000000      # -2.5: -2, NX
	fmv.d.x f23, t0
	fcvtmod.w.d a0, f23, rtz
	FLAGS a1
	li   t0, 0x7FF8000000000000      # quiet NaN: 0, NV
	fmv.d.x f23, t0
	fcvtmod.w.d a0, f23, rtz
	FLAGS a1
	li   t0, 0x0000000000000001      # smallest subnormal: 0, NX
	fmv.d.x f23, t0
	fcvtmod.w.d a0, f23, rtz
	FLAGS a1
	fminm.s f3, f1, f20              # sNaN operand: canonical NaN, NV
	FLAGS a1
	fmv.x.w a0, f3
	fmaxm.d f4, f7, f7               # 2.5
	FLAGS a1
	fmv.x.d a0, f4
	li   t0, 0x7FC00000
	fmv.w.x f24, t0
	fleq.s a0, f1, f24               # quiet NaN: 0, no flag (fle.s would raise NV)
	FLAGS a1
	fltq.s a0, f1, f20               # signaling NaN: 0, NV
	FLAGS a1
	fle.s  a0, f1, f24               # the signaling compare, for contrast: 0, NV
	FLAGS a1

	# ---- 2. FMIN/FMAX with one signaling NaN --------------------------------------
	fmv.w.x f8, zero                 # +0.0f
	li   t0, SNAN_S1
	fmv.w.x f9, t0
	fmin.s f10, f8, f9               # +0.0, NV
	FLAGS a1
	fmv.x.w a0, f10
	fmax.s f10, f9, f8               # +0.0, NV
	FLAGS a1
	fmv.x.w a0, f10
	li   t0, 0x7FC00000              # quiet NaN with the signaling one: canonical NaN, NV
	fmv.w.x f11, t0
	fmin.s f10, f9, f11
	FLAGS a1
	fmv.x.w a0, f10
	li   t0, 0x7FF0000000000001      # double, signaling
	fmv.d.x f12, t0
	li   t0, 0x3FF0000000000000      # 1.0
	fmv.d.x f13, t0
	fmin.d f14, f12, f13             # 1.0, NV
	FLAGS a1
	fmv.x.d a0, f14
	fmax.d f14, f13, f12             # 1.0, NV
	FLAGS a1
	fmv.x.d a0, f14
	li   t0, 0x8000000000000000      # -0.0 against +0.0: min -0, max +0
	fmv.d.x f15, t0
	fmv.d.x f16, zero
	fmin.d f14, f16, f15
	FLAGS a1
	fmv.x.d a0, f14
	fmax.d f14, f15, f16
	FLAGS a1
	fmv.x.d a0, f14

	# ---- 3. vector SEW=32 with a signaling NaN ------------------------------------
	vsetivli zero, 4, e32, m1, ta, ma
	li   t0, SNAN_S
	vmv.v.x v1, t0                   # every element the signaling NaN
	li   t0, ONE_S
	vmv.v.x v2, t0                   # every element 1.0
	fmv.w.x f17, t0                  # 1.0f, for the .vf forms
	li   t0, SNAN_S
	fmv.w.x f18, t0                  # the signaling NaN, as a scalar

	vfadd.vv v3, v1, v2              # canonical NaN, NV
	FLAGS a1
	vfmin.vv v3, v1, v2              # 1.0, NV
	FLAGS a1
	vfmax.vv v3, v2, v1              # 1.0, NV
	FLAGS a1
	vfsqrt.v v3, v1                  # canonical NaN, NV
	FLAGS a1
	vmv.v.v  v3, v2
	vfmacc.vv v3, v1, v2             # canonical NaN, NV
	FLAGS a1
	vmfeq.vv v4, v1, v2              # false, NV: a quiet compare still signals on sNaN
	FLAGS a1
	vfclass.v v3, v1                 # 0x100, signaling NaN
	FLAGS a1
	vfadd.vf v3, v2, f18             # scalar sNaN: canonical NaN, NV
	FLAGS a1
	vfmin.vf v3, v2, f18             # 1.0, NV
	FLAGS a1
	vmv.v.v  v5, v2
	vfredmin.vs v5, v1, v2           # 1.0, NV
	FLAGS a1
	vfredusum.vs v5, v1, v2          # canonical NaN, NV
	FLAGS a1
	vfmv.f.s f19, v1                 # the sNaN, boxed, bit for bit; no flags
	FLAGS a1
	fmv.x.w a0, f19

	# Narrowing conversions, rebuilt on SoftFloat: round-to-odd as SoftFloat's
	# own mode, and float to 8- and 16-bit integers by SoftFloat's narrow
	# conversions (saturating, invalid alone).
	vsetivli zero, 2, e32, m1, ta, ma
	li   t0, 0x3FF0000010000000      # 1 + 2^-24, as a double
	vmv.v.x v6, zero
	vsetivli zero, 1, e64, m1, ta, ma
	vmv.s.x v6, t0
	vsetivli zero, 1, e32, mf2, ta, ma
	vfncvt.rod.f.f.w v7, v6          # 0x3F800001: inexact, the low bit forced
	FLAGS a1
	vfncvt.f.f.w v7, v6              # 0x3F800000 by frm (RNE)
	FLAGS a1
	vsetivli zero, 4, e16, mf2, ta, ma
	li   t0, 0x5BD0                  # 250.0 as a half
	vmv.v.x v8, t0
	vsetivli zero, 4, e8, mf4, ta, ma
	vfncvt.x.f.w  v9, v8             # 127 (saturated), NV
	FLAGS a1
	vfncvt.xu.f.w v9, v8             # 250, exact
	FLAGS a1
	vsetivli zero, 4, e16, mf2, ta, ma
	li   t0, 0x7C00                  # +inf as a half
	vmv.v.x v8, t0
	vfcvt.x.f.v  v9, v8              # 32767, NV
	FLAGS a1
	vsetivli zero, 2, e32, m1, ta, ma
	li   t0, 0x47C35000              # 100000.0f
	vmv.v.x v10, t0
	vsetivli zero, 2, e16, mf2, ta, ma
	vfncvt.xu.f.w v11, v10           # 65535 (saturated), NV
	FLAGS a1
	vfncvt.rtz.x.f.w v11, v10        # 32767 (saturated), NV
	FLAGS a1

	# ---- done -----------------------------------------------------------------------
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	csrr t4, mtval
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

.section .data
.align 3
.global tohost
tohost:   .dword 0
.size tohost, 8
.global fromhost
fromhost: .dword 0
.size fromhost, 8
