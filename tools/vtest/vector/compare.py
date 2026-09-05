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

LAYOUTS = {"vtest_v": LAYOUT_V, "vtest_zb": LAYOUT_ZB}


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
    if len(spike) != len(doomv):
        print("length mismatch: spike=%d doomv=%d" % (len(spike), len(doomv)))

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
