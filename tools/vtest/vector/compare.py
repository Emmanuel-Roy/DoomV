#!/usr/bin/env python3
"""Compare spike.sig against doomv.sig word by word and report the ranges
that differ, annotated with which test in vtest_v.S produced them.

Both files are one 32-bit hex word per line; DoomV writes CRLF on Windows,
so line endings are normalised before comparing.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load(path):
    with open(path) as f:
        return [line.strip().lower() for line in f if line.strip()]


# Signature layout, in order. Each entry is (words, label): SIGX writes 2
# words (one doubleword), SIGV writes 4 (one 128-bit vector register).
LAYOUT = [
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


def main():
    spike = load(os.path.join(HERE, "spike.sig"))
    doomv = load(os.path.join(HERE, "doomv.sig"))
    if len(spike) != len(doomv):
        print("length mismatch: spike=%d doomv=%d" % (len(spike), len(doomv)))

    n = min(len(spike), len(doomv))
    bad = []
    idx = 0
    for words, label in LAYOUT:
        chunk = range(idx, min(idx + words, n))
        diffs = [i for i in chunk if spike[i] != doomv[i]]
        if diffs:
            bad.append((label, idx, words, diffs))
        idx += words

    if idx < n:
        extra = [i for i in range(idx, n) if spike[i] != doomv[i]]
        if extra:
            bad.append(("(past end of layout table)", idx, n - idx, extra))

    if not bad:
        print("MATCH: all %d words identical" % n)
        return 0

    print("MISMATCH in %d of %d tests:\n" % (len(bad), len(LAYOUT)))
    for label, start, words, diffs in bad:
        print("  %s  (words %d..%d)" % (label, start, start + words - 1))
        for i in range(start, min(start + words, n)):
            mark = "  <-- DIFF" if i in diffs else ""
            print("      [%3d] spike=%s doomv=%s%s" % (i, spike[i], doomv[i], mark))
        print("")
    return 1


if __name__ == "__main__":
    sys.exit(main())
