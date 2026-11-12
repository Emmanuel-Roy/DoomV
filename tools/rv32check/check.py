#!/usr/bin/env python3
"""Check DoomV's RV32 results against RV32 semantics.

Runs rv32check.elf on DoomV, then recomputes every result here from the
RISC-V spec (RV32I, M, Zba, Zbb, Zbs, Zbkb) and reports each mismatch.

Usage: python tools/rv32check/check.py DOOMV_EXE RV32CHECK_ELF
"""
import subprocess
import sys

M = 0xFFFFFFFF
def s32(x): x &= M; return x - (1 << 32) if x & 0x80000000 else x

def rol(a, n): n &= 31; return ((a << n) | (a >> (32 - n))) & M if n else a
def ror(a, n): n &= 31; return ((a >> n) | (a << (32 - n))) & M if n else a

def div(a, b):
    a, b = s32(a), s32(b)
    if b == 0: return M
    if a == -(1 << 31) and b == -1: return a & M
    q = abs(a) // abs(b)
    return (q if (a < 0) == (b < 0) else -q) & M

def rem(a, b):
    sa, sb = s32(a), s32(b)
    if sb == 0: return a
    if sa == -(1 << 31) and sb == -1: return 0
    r = abs(sa) % abs(sb)
    return (-r if sa < 0 else r) & M

REF = {
    "add": lambda a, b: (a + b) & M, "sub": lambda a, b: (a - b) & M,
    "sll": lambda a, b: (a << (b & 31)) & M, "srl": lambda a, b: a >> (b & 31),
    "sra": lambda a, b: (s32(a) >> (b & 31)) & M,
    "slt": lambda a, b: int(s32(a) < s32(b)), "sltu": lambda a, b: int(a < b),
    "xor": lambda a, b: a ^ b, "or": lambda a, b: a | b, "and": lambda a, b: a & b,
    "mul": lambda a, b: (a * b) & M,
    "mulh": lambda a, b: ((s32(a) * s32(b)) >> 32) & M,
    "mulhu": lambda a, b: (a * b) >> 32,
    "mulhsu": lambda a, b: ((s32(a) * b) >> 32) & M,
    "div": div, "divu": lambda a, b: a // b if b else M,
    "rem": rem, "remu": lambda a, b: a % b if b else a,
    "sh1add": lambda a, b: ((a << 1) + b) & M, "sh2add": lambda a, b: ((a << 2) + b) & M,
    "sh3add": lambda a, b: ((a << 3) + b) & M,
    "andn": lambda a, b: a & ~b & M, "orn": lambda a, b: (a | ~b) & M, "xnor": lambda a, b: ~(a ^ b) & M,
    "max": lambda a, b: a if s32(a) > s32(b) else b, "maxu": max,
    "min": lambda a, b: a if s32(a) < s32(b) else b, "minu": min,
    "rol": lambda a, b: rol(a, b), "ror": lambda a, b: ror(a, b),
    "clz": lambda a, b: 32 - a.bit_length(),
    "ctz": lambda a, b: 32 if a == 0 else (a & -a).bit_length() - 1,
    "cpop": lambda a, b: bin(a).count("1"),
    "orc_b": lambda a, b: sum((0xFF << (8 * i)) for i in range(4) if (a >> (8 * i)) & 0xFF),
    "rev8": lambda a, b: int.from_bytes(a.to_bytes(4, "little"), "big"),
    "sext_b": lambda a, b: (((a & 0xFF) ^ 0x80) - 0x80) & M,
    "sext_h": lambda a, b: (((a & 0xFFFF) ^ 0x8000) - 0x8000) & M,
    "zext_h": lambda a, b: a & 0xFFFF,
    "bclr": lambda a, b: a & ~(1 << (b & 31)) & M, "bext": lambda a, b: (a >> (b & 31)) & 1,
    "binv": lambda a, b: a ^ (1 << (b & 31)), "bset": lambda a, b: a | (1 << (b & 31)),
    "pack": lambda a, b: (a & 0xFFFF) | ((b & 0xFFFF) << 16),
    "packh": lambda a, b: (a & 0xFF) | ((b & 0xFF) << 8),
    "brev8": lambda a, b: sum(int(f"{(a >> (8 * i)) & 0xFF:08b}"[::-1], 2) << (8 * i) for i in range(4)),
    "srli1": lambda a, b: a >> 1, "srli31": lambda a, b: a >> 31,
    "srai1": lambda a, b: (s32(a) >> 1) & M, "srai31": lambda a, b: (s32(a) >> 31) & M,
    "slli1": lambda a, b: (a << 1) & M, "slli31": lambda a, b: (a << 31) & M,
    "rori1": lambda a, b: ror(a, 1), "rori31": lambda a, b: ror(a, 31),
    "bseti31": lambda a, b: a | 0x80000000, "bclri31": lambda a, b: a & 0x7FFFFFFF,
    "bexti31": lambda a, b: a >> 31, "binvi31": lambda a, b: a ^ 0x80000000,
    "addi_m1": lambda a, b: (a - 1) & M, "sltiu_m1": lambda a, b: int(a < M), "xori_m1": lambda a, b: ~a & M,
}

def main():
    exe, elf = sys.argv[1], sys.argv[2]
    p = subprocess.run([exe, "nowad", elf, "-march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb", "-ng",
                        "-stopat=60000000"], capture_output=True, text=True, errors="replace")
    lines = [l.split() for l in p.stdout.splitlines() if len(l.split()) == 4]
    bad = {}
    for op, a, b, got in lines:
        a, b, got = int(a, 16), int(b, 16), int(got, 16)
        want = REF[op](a, b) & M
        if got != want:
            bad.setdefault(op, []).append((a, b, got, want))
    print(f"rv32check: {len(lines)} results checked, {sum(len(v) for v in bad.values())} wrong")
    for op, cases in bad.items():
        a, b, got, want = cases[0]
        print(f"  {op:8s} {len(cases):3d} wrong, e.g. {a:08x}, {b:08x} -> DoomV {got:08x}, expected {want:08x}")
    if "DONE" not in p.stdout:
        print("rv32check: the program did not finish -- last output:", p.stdout[-300:], p.stderr[-300:])
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
