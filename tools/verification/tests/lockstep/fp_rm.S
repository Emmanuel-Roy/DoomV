# Reserved rounding modes, lock-stepped. Sail makes an FP instruction illegal
# when its rm field is 5 or 6 (they do not decode), when it is dynamic (7)
# and frm holds 5, 6 or 7 (base.reserved_behavior.fcsr_rm = Fcsr_RM_Illegal),
# and makes every vector FP instruction illegal while frm holds 5, 6 or 7.
# DoomV rounded to nearest instead. Each case below either traps or does not;
# lock-step compares the trap, its cause and tval, and every register written.
#
#   rm 5 and 6 in the instruction: FADD.S, FSQRT.D, FMADD.S, FCVT.H.S (Zfh)
#   rm 7 with frm 5, 6, 7: FADD.S, FCVT.W.S, FROUND.S (Zfa) -- illegal
#   rm 7 with frm 4 and 0: legal
#   no rm field, frm reserved: FMIN.S, FSGNJ.S, FCLASS.S -- legal
#   vector FP with frm 5 and 6: VFADD.VV, VFMV.F.S -- illegal
#   vector FP with frm 0, and integer vector with frm 5 -- legal
#
# M-mode throughout; a trap skips the instruction (all are 4 bytes).

.option arch, +f, +d, +v, +zfh, +zfa

.equ MSTATUS_FS, (1 << 13)    # Initial
.equ MSTATUS_VS, (1 << 9)     # Initial

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, MSTATUS_FS | MSTATUS_VS
	csrs mstatus, t0
	li   t0, 0x3FC00000          # 1.5f
	fmv.w.x f1, t0
	li   t0, 0x40100000          # 2.25f
	fmv.w.x f2, t0
	li   t0, 0x4002000000000000  # 2.25
	fmv.d.x f3, t0
	csrwi frm, 0

	# ---- rm 5 and 6 in the instruction --------------------------------------
	.insn r 0x53, 5, 0x00, f4, f1, f2         # fadd.s rm=5
	.insn r 0x53, 6, 0x00, f4, f1, f2         # fadd.s rm=6
	.insn r 0x53, 5, 0x2D, f4, f3, f0         # fsqrt.d rm=5
	.insn r4 0x43, 6, 0, f4, f1, f2, f1       # fmadd.s rm=6
	.insn r 0x53, 5, 0x22, f4, f1, f0         # fcvt.h.s rm=5
	.insn r 0x53, 0, 0x00, f5, f1, f2         # fadd.s rm=0: fine
	fmv.x.w a0, f5

	# ---- dynamic, with frm reserved ------------------------------------------
	csrwi frm, 5
	.insn r 0x53, 7, 0x00, f6, f1, f2         # fadd.s dyn
	.insn r 0x53, 7, 0x60, a1, f1, f0         # fcvt.w.s dyn
	csrwi frm, 6
	.insn r 0x53, 7, 0x20, f6, f1, f4         # fround.s dyn (rs2 = 4)
	csrwi frm, 7
	.insn r 0x53, 7, 0x00, f6, f1, f2         # fadd.s dyn

	# ---- no rm field: frm does not matter -------------------------------------
	fmin.s  f7, f1, f2
	fsgnj.s f8, f2, f1
	fclass.s a2, f1
	fmv.x.w a3, f7
	fmv.x.w a4, f8

	# ---- dynamic, with frm valid ----------------------------------------------
	csrwi frm, 4
	.insn r 0x53, 7, 0x00, f9, f1, f2         # fadd.s dyn, RMM
	fmv.x.w a5, f9
	csrwi frm, 0
	.insn r 0x53, 7, 0x60, a6, f2, f0         # fcvt.w.s dyn, RNE

	# ---- vector -----------------------------------------------------------------
	vsetivli zero, 4, e32, m1, ta, ma
	vfmv.v.f v2, f1
	vfmv.v.f v3, f2
	csrwi frm, 5
	vfadd.vv v1, v2, v3                       # illegal
	vadd.vv  v4, v2, v3                       # integer: fine
	csrwi frm, 6
	vfmv.f.s f10, v2                          # illegal
	csrwi frm, 0
	vfadd.vv v5, v2, v3                       # fine
	vfmv.f.s f11, v5
	fmv.x.w a7, f11

	# ---- done ----------------------------------------------------------------------
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
.global fromhost
fromhost: .dword 0
