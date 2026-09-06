#!/usr/bin/env python3
"""Diff a DoomV signature dump against spike's, word by word, and report each
mismatch annotated with the instruction that produced it.

Usage:  compare.py [test_basename]
  vtest_v   (default) -- the V extension
  vtest_zb            -- the scalar bitmanip families

Reads <test>.spike.sig and <test>.doomv.sig. Both are one 32-bit hex word
per line; DoomV writes CRLF on Windows, so line endings are normalised.

The layout tables below mirror the order of the SIGV/SIGX macros in the
corresponding .S file: SIGX writes 2 words (one doubleword), SIGV writes 4
(one 128-bit vector register). Keeping them in sync is what turns "word 108
differs" into "vmsne.vv is wrong".
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

LAYOUT_V = [
    (2, "vsetivli t0,2,e64,m1"),
    (2, "vsetivli t0,31,e8,m1 (clamp to VLMAX)"),
    (2, "vsetvli t0,100,e8,m1 (clamp)"),
    (2, "vsetvli t0,3,e32,m1"),
    (2, "vsetvli t0,x0,e32,m1 (VLMAX)"),
    (4, "vle8.v"), (4, "vle16.v"), (4, "vle32.v"), (4, "vle64.v"),
    (4, "vadd.vv"), (4, "vadd.vx"), (4, "vadd.vi"),
    (4, "vrsub.vx"), (4, "vrsub.vi"), (4, "vmul.vx"),
    (4, "vand.vv"), (4, "vand.vi"), (4, "vor.vv"), (4, "vnot.v"),
    (4, "vandn.vv (Zvbb)"), (4, "vandn.vx (Zvbb)"),
    (4, "vsll.vi"), (4, "vsll.vx"), (4, "vsrl.vi"), (4, "vsrl.vx"),
    (4, "vmv.v.i"), (4, "vmv.v.x"), (4, "vmv1r.v"),
    (2, "vmv.x.s"), (4, "vmv.s.x"),
    (4, "vid.v"),
    (4, "vmseq.vi"), (4, "vmsne.vv"), (4, "vmor.mm"), (2, "vfirst.m"),
    (4, "vmerge.vim"), (4, "vmerge.vvm"),
    (4, "vrgather.vv"), (4, "vslidedown.vi"),
    (4, "vredsum.vs"), (4, "vredor.vs"),
    (4, "vwaddu.vv"), (4, "vwaddu.wv"), (4, "vnsrl.wi"),
    (4, "vsext.vf4"), (4, "vzext.vf2"),
    (4, "vl1r.v"), (4, "vs1r.v"), (4, "vlm.v"),
    (4, "vlse64.v (strided)"),
    (4, "vluxei64.v (indexed)"),
    (4, "vlseg4e8.v -> v8"), (4, "vlseg4e8.v -> v9"),
    (4, "vlseg4e8.v -> v10"), (4, "vlseg4e8.v -> v11"),
    (4, "vrgatherei16.vv (mixed EEW)"),
    (4, "vle8ff.v (no fault)"), (2, "vl after vle8ff"),
    (4, "vzext.vf8 m8 -> v8"), (4, "vzext.vf8 m8 -> v9"),
]

LAYOUT_ZB = [
    (2, "sh1add"), (2, "sh2add"), (2, "sh3add"),
    (2, "add.uw"), (2, "sh1add.uw"), (2, "sh2add.uw"), (2, "sh3add.uw"),
    (2, "slli.uw 7"), (2, "slli.uw 33"),
    (2, "andn"), (2, "orn"), (2, "xnor"),
    (2, "clz"), (2, "clz(0)"), (2, "clz(msb set)"),
    (2, "ctz"), (2, "ctz(0)"), (2, "ctz(lsb set)"),
    (2, "cpop"), (2, "cpop(0)"),
    (2, "clzw"), (2, "clzw(0)"), (2, "ctzw"), (2, "ctzw(0)"), (2, "cpopw"),
    (2, "min"), (2, "minu"), (2, "max"), (2, "maxu"),
    (2, "sext.b"), (2, "sext.b(neg)"), (2, "sext.h"), (2, "sext.h(neg)"),
    (2, "zext.h"),
    (2, "rol"), (2, "ror"), (2, "rolw"), (2, "rorw"),
    (2, "rori 1"), (2, "rori 63"), (2, "roriw 1"),
    (2, "orc.b"), (2, "orc.b(ffffffff)"), (2, "rev8"),
    (2, "bclr"), (2, "bclri"), (2, "bext"), (2, "bexti"),
    (2, "binv"), (2, "binvi"), (2, "bset"), (2, "bseti 63"),
    (2, "bset (shamt 65 -> 1)"),
    (2, "czero.eqz (rs2!=0)"), (2, "czero.eqz (rs2==0)"),
    (2, "czero.nez (rs2!=0)"), (2, "czero.nez (rs2==0)"),
    (2, "c.zext.b"), (2, "c.sext.b"), (2, "c.zext.h"), (2, "c.sext.h"),
    (2, "c.zext.w"), (2, "c.not"), (2, "c.mul"),
    (2, "c.lbu"), (2, "c.lhu"), (2, "c.lh"), (2, "c.sb/c.sh readback"),
]

LAYOUT_MMU = [
    (2, "Sv39 store/load through a translated VA"),
    (2, "Sv39 second address in the same page"),
    (2, "vl before vle8ff (VLMAX for e8/m1)"),
    (2, "vl after vle8ff crossing into an unmapped page"),
    (2, "vl after vle8ff wholly inside the page"),
    (2, "bytes FOF actually loaded"),
]

# Each trap records three doublewords: scause, stval, vstart.
LAYOUT_TRAP = [
    (2, "load page fault: scause"), (2, "load page fault: stval"),
    (2, "load page fault: vstart"),
    (2, "store page fault: scause"), (2, "store page fault: stval"),
    (2, "store page fault: vstart"),
    (2, "load fault mid-page: scause"), (2, "load fault mid-page: stval"),
    (2, "load fault mid-page: vstart"),
    (2, "ecall from S: scause"), (2, "ecall from S: stval"),
    (2, "ecall from S: vstart"),
    (2, "ebreak: scause"), (2, "ebreak: stval"),
    (2, "ebreak: vstart"),
    (2, "faulting vle8.v: scause"), (2, "faulting vle8.v: stval"),
    (2, "faulting vle8.v: vstart (must be 8)"),
    (2, "still running afterwards"),
]

LAYOUT_RESTART = [
    (2, "vstart seen by the handler (must be 8)"),
    (2, "fault count (must be exactly 1)"),
    (2, "vstart after completion (must be 0)"),
    (4, "v1: both halves, across the restart"),
]

# Every entry is one doubleword: FP results are dumped as raw bits so NaN
# payloads and signed zeros compare exactly.
LAYOUT_FD = [(2, n) for n in [
    "fld 1.5", "fld -2.25", "flw 1.5f", "flw -2.25f",
    "fadd.d", "fsub.d", "fmul.d", "fdiv.d", "fsqrt.d",
    "fadd.s", "fsub.s", "fmul.s", "fdiv.s", "fsqrt.s",
    "fmin.d", "fmax.d", "fmin.d(+0,-0)", "fmax.d(+0,-0)", "fmin.d(NaN)",
    "fmin.s", "fmax.s",
    "fsgnj.d", "fsgnjn.d", "fsgnjx.d", "fsgnj.s",
    "feq.d", "flt.d", "fle.d", "feq.d(NaN,NaN)", "flt.s",
    "fclass.d(1.5)", "fclass.d(-0)", "fclass.d(inf)", "fclass.d(NaN)", "fclass.s",
    "fcvt.w.d", "fcvt.wu.d", "fcvt.l.d", "fcvt.lu.d",
    "fcvt.d.w", "fcvt.d.wu", "fcvt.d.l", "fcvt.s.w", "fcvt.w.s",
    "fcvt.wu.d(neg -> saturate)", "fcvt.w.d(inf)", "fcvt.wu.d(NaN)", "fcvt.l.d(inf)",
    "fcvt.s.d", "fcvt.d.s", "fcvt.s.d(NaN)",
    "fmadd.d", "fmsub.d", "fnmadd.d", "fnmsub.d", "fmadd.s",
    "fmv.x.d", "fmv.d.x", "fmv.x.w",
    "fsd readback", "fsw readback",
    "fflags",
]]

# Zimop is the only member of this group with an observable result (rd must
# read back zero). Everything else is checked by dumping the sentinel
# registers afterwards: a hint that decoded as its host instruction -- PAUSE
# as a FENCE is harmless, but C.NTL as a C.ADD or PREFETCH as an ORI is not
# -- shows up as a corrupted sentinel rather than as a wrong result.
LAYOUT_HINTS = [(2, n) for n in [
    "ld after ntl.all (hint must not disturb the load)",
    "sd/ld after ntl.p1",
    "ld after cbo.clean/flush/inval (data must survive)",
    "mop.r.0 -> rd (must be 0)",
    "mop.r.1 -> rd (must be 0)",
    "mop.r.31 -> rd (must be 0)",
    "mop.rr.0 -> rd (must be 0)",
    "mop.rr.7 -> rd (must be 0)",
    "x0 after mop with rd=x0",
    "a0 sentinel", "a1 sentinel", "a2 sentinel", "a3 sentinel",
    "a4 sentinel", "a5 sentinel", "t0 sentinel", "t1 sentinel",
    "sp (unchanged across every hint)",
]]

# Counter *values* are not comparable between the two machines -- spike
# counts simulated cycles, DoomV counts retired instructions -- so this
# layout deliberately contains no counter value, only the properties both
# must agree on: that counters never go backwards, and that reading one the
# counter-enable chain has not permitted raises an illegal-instruction trap.
LAYOUT_CSR = [(2, n) for n in [
    "cycle monotonic (1)", "time monotonic (1)", "instret monotonic (1)",
    "hpmcounter3 (readable, 0)", "hpmcounter31 (readable, 0)",
    "cbo.zero: block word 0 (aligned down from base+8)",
    "cbo.zero: block word 3", "cbo.zero: block word 7",
    "cbo.zero: guard before block (must survive)",
    "cbo.zero: guard after block (must survive)",
    "wrs.nto/wrs.sto: a0 sentinel", "wrs.nto/wrs.sto: a1 sentinel",
    "S-mode reads with mcounteren=0: trap count (2)",
    "S-mode reads with mcounteren=0: first mcause (2 = illegal)",
    "S-mode reads with mcounteren=7: trap count (0)",
    "write to read-only cycle: trap count (1)",
]]

# Zvbb. Every entry is a whole vector register (4 words), same as
# LAYOUT_V. The e8 repeats of the counting and rotate families are not
# redundant: both are defined over SEW, and the natural wrong
# implementation -- reaching for a 64-bit host operation -- gives the
# right answer at e64 and a wrong one at every narrower width.
LAYOUT_ZVBB = [(4, n) for n in [
    "vandn.vv (e32)",
    "vandn.vx (e32)",
    "vbrev.v (e32)",
    "vbrev8.v (e32)",
    "vrev8.v (e32)",
    "vbrev.v (e8)",
    "vbrev8.v (e8)",
    "vrev8.v (e8)",
    "vclz.v (e8)",
    "vctz.v (e8)",
    "vcpop.v (e8)",
    "vclz.v (e32)",
    "vclz.v (e32, all-zero -> 32)",
    "vctz.v (e32)",
    "vctz.v (e32, all-zero -> 32)",
    "vcpop.v (e32)",
    "vrol.vv (e32)",
    "vrol.vx (e32)",
    "vror.vv (e32)",
    "vror.vx (e32)",
    "vror.vi 7 (e32, imm[5]=0)",
    "vror.vi 31 (e32)",
    "vrol.vv (e8)",
    "vrol.vx (e8)",
    "vror.vv (e8)",
    "vror.vx (e8)",
    "vrol.vx 11 (e8, amount mod SEW)",
    "vror.vi 40 (e64, imm[5]=1 -> funct6 0x15)",
    "vror.vi 63 (e64)",
    "vwsll.vv -> v8",
    "vwsll.vv -> v9",
    "vwsll.vx -> v8",
    "vwsll.vx -> v9",
    "vwsll.vi -> v8",
    "vwsll.vi -> v9",
    "vandn.vv masked (v0.t)",
    "vrol.vv masked (v0.t)",
]]

# Zfa. Generated from the SIGD/SIGS/SIGX/SIGFLAGS order in vtest_zfa.S --
# every entry is one doubleword, FP results dumped as raw bits so NaN
# payloads compare exactly.
#
# The fflags entries are not padding. fleq/fltq return the same value as
# fle/flt for every input and differ only in which NaNs raise invalid, so
# the flags are the only thing that can tell a correct implementation from
# one that aliases them to the base comparisons. Same for fround vs
# froundnx, which differ only in NX.
LAYOUT_ZFA = [(2, n) for n in [
    "fli.d ft0, -1.0",
    "fli.d ft0, min",
    "fli.d ft0, 0x1p-16",
    "fli.d ft0, 0x1p-15",
    "fli.d ft0, 0x1p-8",
    "fli.d ft0, 0x1p-7",
    "fli.d ft0, 0x1p-4",
    "fli.d ft0, 0x1p-3",
    "fli.d ft0, 0.25",
    "fli.d ft0, 0.3125",
    "fli.d ft0, 0.375",
    "fli.d ft0, 0.4375",
    "fli.d ft0, 0.5",
    "fli.d ft0, 0.625",
    "fli.d ft0, 0.75",
    "fli.d ft0, 0.875",
    "fli.d ft0, 1.0",
    "fli.d ft0, 1.25",
    "fli.d ft0, 1.5",
    "fli.d ft0, 1.75",
    "fli.d ft0, 2.0",
    "fli.d ft0, 2.5",
    "fli.d ft0, 3.0",
    "fli.d ft0, 4.0",
    "fli.d ft0, 8.0",
    "fli.d ft0, 16.0",
    "fli.d ft0, 128.0",
    "fli.d ft0, 256.0",
    "fli.d ft0, 32768.0",
    "fli.d ft0, 65536.0",
    "fli.d ft0, inf",
    "fli.d ft0, nan",
    "fli.s ft1, -1.0",
    "fli.s ft1, min",
    "fli.s ft1, 0x1p-16",
    "fli.s ft1, 0.3125",
    "fli.s ft1, 1.0",
    "fli.s ft1, 65536.0",
    "fli.s ft1, inf",
    "fli.s ft1, nan",
    "fminm.d ft0, fa0, fa1",
    "fmaxm.d ft0, fa0, fa1",
    "fminm.s ft1, ft2, ft3",
    "fmaxm.s ft1, ft2, ft3",
    "fflags after fmaxm.s ft1, ft2, ft3",
    "fminm.d ft0, fa0, fa2",
    "fflags after fminm.d ft0, fa0, fa2",
    "fmaxm.d ft0, fa2, fa0",
    "fflags after fmaxm.d ft0, fa2, fa0",
    "fminm.d ft0, fa0, fa3",
    "fflags after fminm.d ft0, fa0, fa3",
    "fminm.s ft1, ft2, ft4",
    "fflags after fminm.s ft1, ft2, ft4",
    "fmin.d ft0, fa0, fa2",
    "fflags after fmin.d ft0, fa0, fa2",
    "fround.d ft0, fa0",
    "fflags after fround.d ft0, fa0",
    "froundnx.d ft0, fa0",
    "fflags after froundnx.d ft0, fa0",
    "fround.d ft0, fa4",
    "fflags after fround.d ft0, fa4",
    "fround.d ft0, fa5",
    "fflags after fround.d ft0, fa5",
    "fround.d ft0, fa1",
    "fflags after fround.d ft0, fa1",
    "froundnx.d ft0, fa1",
    "fflags after froundnx.d ft0, fa1",
    "fround.s ft1, ft2",
    "froundnx.s ft1, ft3",
    "fflags after froundnx.s ft1, ft3",
    "froundnx.d ft0, ft5",
    "fflags after froundnx.d ft0, ft5",
    "fround.d ft0, fa0, rtz",
    "fround.d ft0, fa0, rdn",
    "fround.d ft0, fa0, rup",
    "fround.d ft0, fa1, rtz",
    "fround.d ft0, fa1, rup",
    "fflags after fround.d ft0, fa1, rup",
    "fcvtmod.w.d t0, ft6, rtz",
    "fflags after fcvtmod.w.d t0, ft6, rtz",
    "fcvtmod.w.d t0, ft6, rtz",
    "fflags after fcvtmod.w.d t0, ft6, rtz",
    "fcvtmod.w.d t0, ft6, rtz",
    "fflags after fcvtmod.w.d t0, ft6, rtz",
    "fcvtmod.w.d t0, ft6, rtz",
    "fflags after fcvtmod.w.d t0, ft6, rtz",
    "fcvtmod.w.d t0, fa0, rtz",
    "fflags after fcvtmod.w.d t0, fa0, rtz",
    "fcvtmod.w.d t0, fa1, rtz",
    "fflags after fcvtmod.w.d t0, fa1, rtz",
    "fcvtmod.w.d t0, fa2, rtz",
    "fflags after fcvtmod.w.d t0, fa2, rtz",
    "fcvtmod.w.d t0, ft6, rtz",
    "fflags after fcvtmod.w.d t0, ft6, rtz",
    "fcvtmod.w.d t0, ft6, rtz",
    "fflags after fcvtmod.w.d t0, ft6, rtz",
    "fcvt.w.d t0, ft6, rtz",
    "fflags after fcvt.w.d t0, ft6, rtz",
    "fleq.d t0, fa0, fa1",
    "fltq.d t0, fa0, fa1",
    "fleq.d t0, fa1, fa0",
    "fltq.d t0, fa1, fa0",
    "fleq.d t0, fa0, fa0",
    "fflags after fleq.d t0, fa0, fa0",
    "fleq.d t0, fa0, fa2",
    "fflags after fleq.d t0, fa0, fa2",
    "fltq.d t0, fa2, fa0",
    "fflags after fltq.d t0, fa2, fa0",
    "fle.d t0, fa0, fa2",
    "fflags after fle.d t0, fa0, fa2",
    "fleq.d t0, fa0, fa3",
    "fflags after fleq.d t0, fa0, fa3",
    "fleq.s t0, ft2, ft3",
    "fltq.s t0, ft2, ft4",
    "fflags after fltq.s t0, ft2, ft4",
]]

# Zfhmin. Generated from the SIG* order in vtest_zfh.S. Every entry is one
# doubleword; half and single results are dumped as raw bits.
#
# The fflags entries carry most of the weight here. Underflow, inexact and
# overflow are what separate a correct narrowing conversion from one that
# lands on the same value by luck -- and since MinGW has no _Float16, every
# conversion is hand-written integer code rather than a host FPU op.
LAYOUT_ZFH = [(2, n) for n in [
    "flh ft0, 0(s1)",
    "flh ft0, 2(s1)",
    "flh ft0, 4(s1)",
    "flh ft0, 6(s1)",
    "flh ft0, 8(s1)",
    "flh ft0, 10(s1)",
    "flh ft0, 12(s1)",
    "flh ft0, 14(s1)",
    "flh ft0, 16(s1)",
    "fflags after flh ft0, 16(s1)",
    "fmv.h.x ft1, t1",
    "lhu t3, 0(t2)",
    "fmv.h.x ft1, t1",
    "fflags after fmv.h.x ft1, t1",
    "fcvt.s.h ft2, ft0",
    "fcvt.s.h ft2, ft0",
    "fcvt.s.h ft2, ft0",
    "fcvt.s.h ft2, ft0",
    "fcvt.s.h ft2, ft0",
    "fcvt.s.h ft2, ft0",
    "fcvt.s.h ft2, ft0",
    "fflags after fcvt.s.h ft2, ft0",
    "fcvt.d.h ft3, ft0",
    "fcvt.d.h ft3, ft0",
    "fcvt.d.h ft3, ft0",
    "fflags after fcvt.d.h ft3, ft0",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2",
    "fflags after fcvt.h.s ft0, ft2",
    "fcvt.h.s ft0, ft2, rne",
    "fcvt.h.s ft0, ft2, rtz",
    "fcvt.h.s ft0, ft2, rdn",
    "fcvt.h.s ft0, ft2, rup",
    "fflags after fcvt.h.s ft0, ft2, rup",
    "fcvt.h.s ft0, ft2, rne",
    "fcvt.h.s ft0, ft2, rtz",
    "fcvt.h.s ft0, ft2, rdn",
    "fcvt.h.s ft0, ft2, rup",
    "fflags after fcvt.h.s ft0, ft2, rup",
    "fcvt.h.s ft0, ft2, rne",
    "fcvt.h.s ft0, ft2, rtz",
    "fcvt.h.s ft0, ft2, rdn",
    "fcvt.h.s ft0, ft2, rup",
    "fflags after fcvt.h.s ft0, ft2, rup",
    "fcvt.h.d ft0, ft3",
    "fflags after fcvt.h.d ft0, ft3",
    "fcvt.h.d ft0, ft3",
    "fflags after fcvt.h.d ft0, ft3",
    "fcvt.h.d ft0, ft3",
    "fflags after fcvt.h.d ft0, ft3",
    "fcvt.h.d ft0, ft3",
    "fflags after fcvt.h.d ft0, ft3",
    "fcvt.h.d ft0, ft3",
    "fflags after fcvt.h.d ft0, ft3",
    "fcvt.h.s ft1, ft2",
    "fcvt.h.s ft1, ft2",
    "fcvt.h.s ft1, ft2",
    "fflags after fcvt.h.s ft1, ft2",
]]

# The vector float conversion family. Widening/narrowing entries are a
# whole vector register (4 words); fflags entries are one doubleword.
#
# Every instruction here was a no-op before this suite existed: VFUNARY0
# accepted only the same-width subcodes and returned for the rest, leaving
# the destination register untouched with no trap. So a mismatch on any of
# these is not a rounding disagreement -- it means the instruction did
# nothing at all.
LAYOUT_ZVFH = [
    (4, "vfwcvt.f.f.v v8, v1 -> v8"),
    (4, "vfwcvt.f.f.v v8, v1 -> v9"),
    (2, "fflags after vfwcvt.f.f.v v8, v1"),
    (4, "vfncvt.f.f.w v2, v8 -> v2"),
    (2, "fflags after vfncvt.f.f.w v2, v8"),
    (4, "vfwcvt.f.f.v v8, v1 -> v8"),
    (4, "vfwcvt.f.f.v v8, v1 -> v9"),
    (2, "fflags after vfwcvt.f.f.v v8, v1"),
    (4, "vfncvt.f.f.w v2, v8 -> v2"),
    (2, "fflags after vfncvt.f.f.w v2, v8"),
    (4, "vfncvt.rod.f.f.w v3, v8 -> v3"),
    (2, "fflags after vfncvt.rod.f.f.w v3, v8"),
    (4, "vfwcvt.f.x.v v8, v1 -> v8"),
    (4, "vfwcvt.f.x.v v8, v1 -> v9"),
    (2, "fflags after vfwcvt.f.x.v v8, v1"),
    (4, "vfwcvt.f.xu.v v8, v1 -> v8"),
    (4, "vfwcvt.f.xu.v v8, v1 -> v9"),
    (2, "fflags after vfwcvt.f.xu.v v8, v1"),
    (4, "vfwcvt.x.f.v v8, v1 -> v8"),
    (4, "vfwcvt.x.f.v v8, v1 -> v9"),
    (2, "fflags after vfwcvt.x.f.v v8, v1"),
    (4, "vfwcvt.rtz.x.f.v v8, v1 -> v8"),
    (4, "vfwcvt.rtz.x.f.v v8, v1 -> v9"),
    (2, "fflags after vfwcvt.rtz.x.f.v v8, v1"),
    (4, "vfwcvt.rtz.xu.f.v v8, v1 -> v8"),
    (4, "vfwcvt.rtz.xu.f.v v8, v1 -> v9"),
    (2, "fflags after vfwcvt.rtz.xu.f.v v8, v1"),
    (4, "vfncvt.x.f.w v2, v8 -> v2"),
    (2, "fflags after vfncvt.x.f.w v2, v8"),
    (4, "vfncvt.rtz.x.f.w v2, v8 -> v2"),
    (2, "fflags after vfncvt.rtz.x.f.w v2, v8"),
    (4, "vfncvt.rtz.xu.f.w v2, v8 -> v2"),
    (2, "fflags after vfncvt.rtz.xu.f.w v2, v8"),
    (4, "vfncvt.f.x.w v2, v8 -> v2"),
    (2, "fflags after vfncvt.f.x.w v2, v8"),
    (4, "vfncvt.f.xu.w v2, v8 -> v2"),
    (2, "fflags after vfncvt.f.xu.w v2, v8"),
    (4, "vfwcvt.f.f.v v8, v1, v0.t -> v8"),
    (4, "vfwcvt.f.f.v v8, v1, v0.t -> v9"),
    (2, "fflags after vfwcvt.f.f.v v8, v1, v0.t"),
]

# The supervisor translation extensions. Svnapot and Svpbmt add no
# instructions -- they give meaning to PTE bits -- so every entry here is
# the observable consequence of a page-table walk rather than an
# instruction result.
#
# The fault counts carry the weight. Svpbmt's memory types have no effect
# on a machine with no caches, so the only way to tell a real
# implementation from one that ignores the field is that the reserved
# value, and any value at all while menvcfg.PBMTE is clear, must fault.
LAYOUT_SV = [(2, n) for n in [
    "Svnapot: first page of the 64KB range",
    "Svnapot: last page of the range (must not alias the first)",
    "Svnapot: middle page",
    "Svnapot: first page still intact after the others",
    "Svinval: mapping unchanged after the invalidation sequence",
    "Svinval: fault count (must be 0)",
    "Svpbmt NC: value read back",
    "Svpbmt NC: fault count (must be 0)",
    "Svpbmt reserved type 3: fault count (must be 1)",
    "Svpbmt reserved type 3: scause (13 = load page fault)",
    "Svpbmt with PBMTE clear: fault count (must be 1)",
    "Svpbmt with PBMTE clear: scause",
    "PTE reserved bit 54: fault count (must be 1)",
    "PTE reserved bit 54: scause",
    "Svnapot bad size encoding: fault count (must be 1)",
    "Svnapot bad size encoding: scause",
]]

# Pointer masking. Address arithmetic, so a wrong implementation does not
# fault -- it reaches a different address and succeeds. Every entry here is
# therefore either a value written through a tagged pointer and read back
# through a clean one, or a fault count proving the tag still mattered when
# masking was off.
LAYOUT_PM = [(2, n) for n in [
    "masking off: clean sentinel untouched by the tagged store",
    "PMLEN=7: value read back through the clean pointer",
    "PMLEN=7: fault count (0)",
    "PMLEN=7: clean store, tagged load",
    "PMLEN=7: a different tag reaches the same address",
    "sign extension: sentinel intact (a zeroing impl would corrupt it)",
    "senvcfg does not govern S-mode: sentinel intact",
    "U-mode (Ssnpm): value read back",
    "U-mode (Ssnpm): fault count (0)",
    "PMLEN=16: value read back",
    "PMLEN=16: fault count (0)",
    "PMM=1 reserved behaves as off: sentinel intact",
]]

# The hypervisor extension's CSR file and privilege plumbing -- H's first
# increment, before two-stage translation exists.
#
# The cause numbers carry most of the weight. A guest reaching for the
# hypervisor's registers must raise 22 (virtual instruction) and not 2
# (illegal), because that is what lets a hypervisor emulate the access
# rather than kill the guest -- and the read-only-CSR case is here to show
# 2 still happens, so the two are genuinely distinguished.
LAYOUT_H = [(2, n) for n in [
    "misa.H advertised (1)",
    "hstatus after writing all-ones (WARL: reserved bits read zero)",
    "hstatus after writing zero (VSXL still reads 2)",
    "vstvec read/write from HS-mode",
    "vsscratch read/write from HS-mode",
    "guest writes 'stvec': reads back its own value",
    "guest writes 'sscratch': reads back its own value",
    "guest reads hstatus: trap count (1)",
    "guest reads hstatus: cause (22 = virtual instruction)",
    "guest reads vstvec by name: trap count (1)",
    "guest reads vstvec by name: cause (22)",
    "guest reads hgatp: trap count (1)",
    "guest reads hgatp: cause (22)",
    "guest writes read-only cycle: trap count (1)",
    "guest writes read-only cycle: cause (2 = illegal, not 22)",
    "ecall from VS-mode: cause (10, not 9)",
    "mstatus.MPV recorded by the trap (1)",
    "mstatus.MPP recorded by the trap (1 = S)",
    "hypervisor's sscratch survived the guest (0x1111)",
    "guest's sscratch write landed in vsscratch (0x3333)",
    "guest's stvec write landed in vstvec",
    "hypervisor's stvec unchanged (0)",
]]

LAYOUTS = {
    "vtest_v": LAYOUT_V,
    "vtest_zb": LAYOUT_ZB,
    "vtest_mmu": LAYOUT_MMU,
    "vtest_trap": LAYOUT_TRAP,
    "vtest_restart": LAYOUT_RESTART,
    "vtest_fd": LAYOUT_FD,
    "vtest_hints": LAYOUT_HINTS,
    "vtest_csr": LAYOUT_CSR,
    "vtest_zvbb": LAYOUT_ZVBB,
    "vtest_zfa": LAYOUT_ZFA,
    "vtest_zfh": LAYOUT_ZFH,
    "vtest_zvfh": LAYOUT_ZVFH,
    "vtest_sv": LAYOUT_SV,
    "vtest_pm": LAYOUT_PM,
    "vtest_h": LAYOUT_H,
}


def load(path):
    with open(path) as f:
        return [line.strip().lower() for line in f if line.strip()]


def main():
    test = sys.argv[1] if len(sys.argv) > 1 else "vtest_v"
    if test not in LAYOUTS:
        print("unknown test %r (known: %s)" % (test, ", ".join(sorted(LAYOUTS))))
        return 2
    layout = LAYOUTS[test]

    spike = load(os.path.join(HERE, test + ".spike.sig"))
    doomv = load(os.path.join(HERE, test + ".doomv.sig"))

    # An empty side means the run never produced a signature at all -- most
    # often spike sitting in a trap loop so its `until pc` never matched, in
    # which case it emits nothing and exits 0. Comparing zero words against
    # zero words otherwise reports a cheerful MATCH, which is the single most
    # misleading thing this script could do.
    if not spike or not doomv:
        print("NO DATA: spike=%d words, doomv=%d words -- the run produced no"
              " signature, so nothing was compared" % (len(spike), len(doomv)))
        return 2
    if len(spike) != len(doomv):
        print("LENGTH MISMATCH: spike=%d doomv=%d -- comparing the common"
              " prefix only" % (len(spike), len(doomv)))

    n = min(len(spike), len(doomv))
    bad = []
    idx = 0
    for words, label in layout:
        diffs = [i for i in range(idx, min(idx + words, n)) if spike[i] != doomv[i]]
        if diffs:
            bad.append((label, idx, words, diffs))
        idx += words

    # Anything past the layout table is the zero tail of the signature
    # region; it should still match, and a difference there means the test
    # wrote more than the table accounts for.
    if idx < n:
        extra = [i for i in range(idx, n) if spike[i] != doomv[i]]
        if extra:
            bad.append(("(past end of layout table)", idx, n - idx, extra))

    if not bad:
        if len(spike) != len(doomv):
            print("%s: common prefix of %d words matches, but the two dumps are"
                  " different lengths" % (test, n))
            return 1
        print("MATCH: %s -- all %d words identical (%d tests)" % (test, n, len(layout)))
        return 0

    print("MISMATCH in %d of %d tests:\n" % (len(bad), len(layout)))
    for label, start, words, diffs in bad:
        print("  %s  (words %d..%d)" % (label, start, start + words - 1))
        for i in range(start, min(start + words, n)):
            mark = "  <-- DIFF" if i in diffs else ""
            print("      [%3d] spike=%s doomv=%s%s" % (i, spike[i], doomv[i], mark))
        print("")
    return 1


if __name__ == "__main__":
    sys.exit(main())
