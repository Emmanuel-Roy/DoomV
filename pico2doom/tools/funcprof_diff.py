#!/usr/bin/env python3
"""Lock-step drill-down: which functions retired different instruction counts.

Both builds made with -DPORT_FUNCPROF=ON dump, for every big game-logic
window, "funcprof <frame> <address> <instructions>" per function. In a DoomV
replay the Pico's dump arrives as "picolink: Pico says: funcprof ..." lines
and DoomV's own as plain "funcprof ..." lines, so one log has both.

Usage: python tools/funcprof_diff.py DOOMV_LOG PICO_ELF DOOMV_ELF
"""
import re
import subprocess
import sys
from collections import defaultdict


def names(elf):
    out = subprocess.run(["riscv-none-elf-nm", elf], capture_output=True, text=True).stdout
    m = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTW":
            m[int(p[0], 16)] = p[2]
    return m


def main():
    log, pico_elf, doomv_elf = sys.argv[1:4]
    pn, dn = names(pico_elf), names(doomv_elf)
    pico, doomv = defaultdict(dict), defaultdict(dict)
    merged = defaultdict(set)   # slots two functions shared, in either build: not comparable
    for line in open(log, errors="replace"):
        m = re.search(r"(Pico says: |Pico: )?funcprof (\d+) ([0-9a-f]+) (\d+)( merged)?", line)
        if not m:
            continue
        frame, addr, n = int(m.group(2)), int(m.group(3), 16), int(m.group(4))
        side, nm = (pico, pn) if m.group(1) else (doomv, dn)
        name = nm.get(addr, hex(addr))
        side[frame][name] = n
        if m.group(5):
            merged[frame].add(name)
    for frame in sorted(set(pico) & set(doomv)):
        p, d = pico[frame], doomv[frame]
        clean = (set(p) | set(d)) - merged[frame]
        diffs = sorted((f for f in clean if p.get(f, 0) != d.get(f, 0)),
                       key=lambda f: -abs(p.get(f, 0) - d.get(f, 0)))
        print(f"frame {frame}: {sum(p.values()):,} instructions on the Pico, {sum(d.values()):,} on DoomV; "
              f"{len(diffs)} functions differ (of {len(clean)} comparable; {len(merged[frame])} in shared slots)")
        for f in diffs[:25]:
            print(f"  {f:32s} Pico {p.get(f, 0):10,}  DoomV {d.get(f, 0):10,}  ({d.get(f, 0) - p.get(f, 0):+d})")
    if not (set(pico) & set(doomv)):
        print("no frame was dumped by both machines (Pico frames: %s, DoomV frames: %s)"
              % (sorted(pico)[:10], sorted(doomv)[:10]))


if __name__ == "__main__":
    main()
