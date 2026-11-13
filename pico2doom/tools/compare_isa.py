#!/usr/bin/env python3
"""Check that the Pico firmware and the DoomV guest are identical builds.

Lock-step compares *retired instruction counts* per frame between a real
Hazard3 core and DoomV, so every instruction DOOM can execute inside a
counted window must be the same in both images. This disassembles both ELFs
and checks, strictly:

  1. Every function reachable from the counted windows (game logic and
     rendering, see port/port_perf.c) is present in both images and is
     instruction-for-instruction identical -- opcodes, registers, and the
     names of the functions it calls. Reachability follows the call graph
     down through the C library, stopping only where the runtime measures
     the platform's instructions and subtracts them (the bodies of
     DG_GetTicksMs/DG_GetKey, the console behind _write) or where DOOM stops
     (the fatal-error screen).
  2. As a summary, how many of all the functions the two images share are
     identical (the rest are the platform backends).

Addresses legitimately differ (the Pico runs from flash at 0x10000000, the
DoomV guest from RAM at 0x80000000), so numbers are ignored. Two spellings
objdump uses for the same instruction are folded together: `jalr ra` is
`jalr 0(ra)` (and `jr t1` is `jr 0(t1)`), and `mv a,b` is `addi a,b,0`.

Exit status 0 only if (1) holds -- run it after every build.

Usage: python tools/compare_isa.py build/firmware/pico2doom.elf build/doomv/pico2doom-doomv.elf
"""
import re
import subprocess
import sys

# Entry points of the counted windows (port/port_perf.c's TIMED calls).
ROOTS = ["__real_G_Ticker", "__real_R_RenderBSPNode", "__real_R_DrawPlanes",
         "__real_R_DrawMasked", "__real_ST_Drawer"]
# Where counting deliberately stops: platform code measured and subtracted
# at run time (port_link.c, port_libc.c), and the fatal-error path, after
# which nothing is counted again.
# backend_heap runs once, on the first _sbrk, which port_run makes before
# DOOM starts (port_libc_init).
EXCLUDED = {"ticks_body", "key_body", "backend_console", "backend_fatal", "backend_heap"}
CALLS = {"jal", "jalr", "jr", "j", "call", "tail"}


def symbol_ends(elf, objdump):
    """Each function's end address, from the ELF symbol table: objdump keeps
    disassembling past a function into whatever data follows it."""
    nm = objdump.replace("objdump", "nm")
    out = subprocess.run([nm, "-S", elf], capture_output=True, text=True, check=True).stdout
    ends = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "tTW":
            ends[p[3]] = int(p[0], 16) + int(p[1], 16)
    return ends


def functions(elf, objdump):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    ends = symbol_ends(elf, objdump)
    code, callees, cur, end = {}, {}, None, None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = m.group(1)
            end = ends.get(cur)
            code[cur], callees[cur] = [], set()
            continue
        m = re.match(r"^\s*([0-9a-f]+):\t(\S+)\s*(.*)", line)
        if not (m and cur):
            continue
        if end is not None and int(m.group(1), 16) >= end:
            continue                                     # past the symbol: data, not code
        op, args = m.group(2), m.group(3)
        target = re.search(r"<([^>+]+)>", args)        # a whole function, not func+0x..
        if op in CALLS and target and target.group(1) != cur:
            callees[cur].add(target.group(1))
            tag = "->" + target.group(1)
        else:
            tag = ""
        args = re.sub(r"#.*|<[^>]*>", "", args)
        args = re.sub(r"-?\b(0x)?[0-9a-f]+\b", "N", args).strip()
        if op == "mv":                                   # mv rd, rs == addi rd, rs, 0
            op, args = "addi", args + ",N"
        if op in ("jalr", "jr") and "(" not in args:     # jalr rs == jalr 0(rs)
            args = "N(%s)" % args
        code[cur].append(op + " " + args + tag)
    return code, callees


def reachable(callees, roots):
    seen, todo = set(), list(roots)
    while todo:
        f = todo.pop()
        if f in seen or f in EXCLUDED:
            continue
        seen.add(f)
        todo += sorted(callees.get(f, ()))
    return seen


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    objdump = "riscv-none-elf-objdump"
    pico, pico_calls = functions(sys.argv[1], objdump)
    doomv, doomv_calls = functions(sys.argv[2], objdump)

    ok = True
    reach_p, reach_d = reachable(pico_calls, ROOTS), reachable(doomv_calls, ROOTS)
    only_p, only_d = sorted(reach_p - reach_d), sorted(reach_d - reach_p)
    differ = sorted(f for f in reach_p & reach_d if f in pico and f in doomv and pico[f] != doomv[f])
    counted = sum(len(pico[f]) for f in reach_p & reach_d if f in pico)
    print(f"reachable from DOOM's counted windows: {len(reach_p & reach_d)} functions, "
          f"{counted:,} instructions")
    if only_p or only_d or differ:
        ok = False
        if only_p: print("  only in the Pico build: " + ", ".join(only_p))
        if only_d: print("  only in the DoomV build: " + ", ".join(only_d))
        if differ: print("  DIFFERENT in the two builds: " + ", ".join(differ))
    else:
        print("  all identical in both builds (instructions and call targets)")

    common = set(pico) & set(doomv)
    same = [f for f in common if pico[f] == doomv[f]]
    print(f"all functions in both images: {len(common)}, identical: {len(same)} "
          f"(the rest are platform backends)")
    if ok:
        print("OK: the builds are identical wherever DOOM's retired instructions are counted.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
