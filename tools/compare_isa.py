#!/usr/bin/env python3
"""Check that the Pico firmware and the DoomV guest run the same instructions.

Lock-step compares *retired instruction counts* per frame between a real
Hazard3 core and DoomV, so the two images must contain the same machine code
for everything that is counted. This disassembles both ELFs and compares
every function present in both, instruction by instruction.

Addresses legitimately differ (the Pico runs from flash at 0x10000000, the
DoomV guest from RAM at 0x80000000), so numbers are ignored: what must match
is each instruction's opcode and registers, in order. Two spellings objdump
uses for the same instruction are folded together: `jalr ra` is
`jalr 0(ra)` (and `jr t1` is `jr 0(t1)`), and `mv a,b` is `addi a,b,0`.

Usage: python tools/compare_isa.py build/firmware/pico2doom.elf build/doomv/pico2doom-doomv.elf
"""
import re
import subprocess
import sys

def functions(elf, objdump):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
            continue
        m = re.match(r"^\s*[0-9a-f]+:\t(\S+)\s*(.*)", line)
        if not (m and cur):
            continue
        op, args = m.group(1), m.group(2)
        args = re.sub(r"#.*|<[^>]*>", "", args)
        args = re.sub(r"-?\b(0x)?[0-9a-f]+\b", "N", args).strip()
        if op == "mv":                       # mv rd, rs  ==  addi rd, rs, 0
            op, args = "addi", args + ",N"
        if op in ("jalr", "jr") and "(" not in args:  # jalr rs == jalr 0(rs); jr likewise
            args = "N(%s)" % args
        funcs[cur].append(op + " " + args)
    return funcs

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    objdump = "riscv-none-elf-objdump"
    pico, doomv = functions(sys.argv[1], objdump), functions(sys.argv[2], objdump)
    common = sorted(set(pico) & set(doomv))
    differ = [f for f in common if pico[f] != doomv[f]]
    # Meant to differ: the platform backends; the C library's system-call
    # layer (pico-sdk provides its own _write/_sbrk/exit...; the DoomV guest
    # uses newlib's stubs); libgcc's soft-float, which objdump splits at
    # different symbols in the two images; and __clz_tab, a data table
    # objdump disassembles anyway. DOOM only reaches the system calls for
    # printf and file I/O (saving a game), so a tic that saves can retire a
    # different count on the two machines while its hashes still agree.
    expected = re.compile(r"^(backend_|main$|mkdir$|_exit$|exit$|_(close|fstat|isatty|lseek|open|read|sbrk|unlink|write)$"
                          r"|__assert_func$|__clz_tab$|__\w+[sdt]f[23]$)")
    unexpected = [f for f in differ if not expected.match(f)]
    insns = sum(len(pico[f]) for f in common if f not in differ)
    print(f"functions in both images: {len(common)}")
    print(f"identical instruction sequences: {len(common) - len(differ)} ({insns:,} instructions)")
    print(f"different, as expected (platform backends): {len(differ) - len(unexpected)}")
    if unexpected:
        print("DIFFERENT, unexpectedly:", ", ".join(unexpected))
        sys.exit(1)
    print("OK: DOOM and the port layer are the same machine code on the Pico and on DoomV.")

if __name__ == "__main__":
    main()
