#!/usr/bin/env python3
"""Run the official riscv-arch-test RVA23S64 suite against DoomV, with Sail
as the reference model.

This is the systematic counterpart to the hand-written differential suites in
../differential/vector. Those cover what I thought to test; this covers what
the architecture's own certification suite tests, which is what makes a claim
like "matches Sail" mean something.

Sail is the reference here, not a second opinion: riscv-arch-test ships no
spike RVA23S64 configuration at all, so at profile level Sail is the only
reference that exists.

Reference signatures are produced by `act` (see gen_reference.sh) into
work/<config>/build/**/<test>.sig, alongside the <test>.sig.elf they came
from. This script runs DoomV over the same ELF and diffs the two.

Symbols are read out of the ELF here rather than shelled out to nm: the work
tree is on the Windows filesystem while the toolchain lives in WSL, and a
round trip per test would dominate the runtime of the whole suite.

  python archtest.py                 # every test act has built
  python archtest.py Zicond Zba      # only these extension directories
  python archtest.py --limit 20      # first 20, for a smoke run
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import time
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
CONFIG = "sail-RVA23S64"

# DoomV's -march resets every extension it does not name, so the profile has
# to be spelled out in full rather than added to a default.
MARCH = (
    "rv64imafdcvh_zicsr_zifencei_zba_zbb_zbs_zicond_zihintpause_zihintntl"
    "_zimop_zcmop_zicbom_zicbop_zicboz_zawrs_zfa_zfh_svinval"
    "_svnapot_svpbmt_sscofpmf_ssstateen_ssnpm_smnpm_sspm"
)


def elf_symbols(path: Path) -> dict:
    """Return {name: value} for a little-endian ELF64 file's symbol table.

    Deliberately minimal -- enough for begin_signature/end_signature and the
    HTIF exit points, and nothing else. A malformed file raises rather than
    returning a partial table, because a silently empty symbol table would
    look exactly like a test with no signature region.
    """
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError("not a little-endian ELF64")
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x3A)

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        fields = struct.unpack_from("<IIQQQQIIQQ", data, off)
        # name, type, flags, addr, offset, size, link, info, align, entsize
        sections.append(fields)

    syms = {}
    for sec in sections:
        stype, s_off, s_size, s_link, s_entsize = sec[1], sec[4], sec[5], sec[6], sec[9]
        if stype not in (2, 11) or s_entsize == 0:      # SHT_SYMTAB / SHT_DYNSYM
            continue
        link = sections[s_link]
        strtab = data[link[4]:link[4] + link[5]]
        for off in range(s_off, s_off + s_size, s_entsize):
            st_name, _info, _other, _shndx, st_value, _size = struct.unpack_from(
                "<IBBHQQ", data, off)
            if st_name == 0:
                continue
            end = strtab.index(b"\0", st_name)
            syms[strtab[st_name:end].decode(errors="replace")] = st_value
    return syms


def read_sail_sig(path: Path) -> list:
    """Sail writes one 64-bit word per line."""
    return [int(w, 16) for w in path.read_text().split()]


def read_doomv_sig(path: Path) -> list:
    """DoomV writes one 32-bit word per line, low word of each doubleword
    first. Recombine so both sides are compared as doublewords -- comparing
    at different widths is how a byte-order bug hides.
    """
    words = [int(w, 16) for w in path.read_text().split()]
    if len(words) % 2:
        words.append(0)
    return [words[i] | (words[i + 1] << 32) for i in range(0, len(words), 2)]


def run_one(elf: Path, sail_sig: Path, outdir: Path, keep: bool, timeout: int) -> tuple:
    """Returns (status, detail); status in {pass, fail, nosig, skip}."""
    name = elf.name[: -len(".sig.elf")]
    try:
        syms = elf_symbols(elf)
    except (ValueError, struct.error) as e:
        return "skip", str(e)
    beg, end = syms.get("begin_signature"), syms.get("end_signature")
    if beg is None or end is None:
        return "skip", "no signature symbols"

    # The test writes its verdict to tohost and spins; DoomV does not exit on
    # an HTIF write, so it is stopped at the store instead.
    halt = syms.get("write_tohost_pass") or syms.get("write_tohost")

    siglog = ROOT / "signature.log"
    cmd = [
        str(ROOT / "riscv_doom.exe"),
        str(ROOT / "tools" / "doom" / "doombuild" / "DOOM1.WAD"),
        str(elf),
        "-march=" + MARCH,
        "-sig={:x}:{:x}".format(beg, end),
    ]
    if halt:
        cmd.append("-break=0x{:x}".format(halt))

    # DoomV hardcodes its -sig output to ./signature.log relative to its own
    # cwd, so this run and the differential harness next door would silently
    # overwrite each other's results -- which looks like a wrong answer, not
    # like a collision. Same lock directory as run_diff.sh; mkdir is atomic
    # even over a Windows filesystem, where flock is not dependable.
    lock = ROOT / ".signature.lock"
    for _ in range(600):
        try:
            lock.mkdir()
            break
        except FileExistsError:
            time.sleep(1)
    else:
        return "nosig", "timed out waiting for the signature lock"
    try:
        if siglog.exists():
            siglog.unlink()
        # A timeout is the normal ending here, not a failure. DoomV writes
        # the signature when it reaches the -break address and then keeps its
        # SDL window alive rather than exiting, so the process always has to
        # be killed. What decides the outcome is whether signature.log
        # appeared -- exactly as run_diff.sh does next door. Treating the
        # timeout itself as the verdict reported every passing test as
        # "timed out", which is how this looked at first.
        try:
            subprocess.run(cmd, cwd=str(ROOT), timeout=timeout,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            pass
        if not siglog.exists():
            return "nosig", "no signature.log (never reached the halt address)"
        # Claim the file while still holding the lock. Releasing first would
        # leave a window in which another run's DoomV overwrites it, and the
        # result would be attributed to this test.
        dut = outdir / (name + ".doomv.sig")
        if dut.exists():
            dut.unlink()
        siglog.replace(dut)
    finally:
        try:
            lock.rmdir()
        except OSError:
            pass

    ref = read_sail_sig(sail_sig)
    got = read_doomv_sig(dut)
    n = min(len(ref), len(got))
    diffs = [i for i in range(n) if ref[i] != got[i]]
    if not diffs and len(ref) == len(got):
        if not keep:
            dut.unlink()
        return "pass", ""
    if diffs:
        i = diffs[0]
        return "fail", "{}/{} words differ, first at [{}] sail={:016x} doomv={:016x}".format(
            len(diffs), n, i, ref[i], got[i])
    return "fail", "length {} vs sail {}".format(len(got), len(ref))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("extensions", nargs="*", help="limit to these extension directories")
    ap.add_argument("--work", default=str(HERE / "work"))
    ap.add_argument("--keep", action="store_true", help="keep passing signatures")
    ap.add_argument("--limit", type=int, default=0)
    # The arch-test binaries are far larger than the hand-written
    # differential tests -- a 124KB signature region against roughly 1KB --
    # so the per-test budget is minutes, not seconds. Too low a value does
    # not fail loudly: it reports "timed out", which reads like a hang.
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()

    build = Path(args.work) / CONFIG / "build"
    if not build.is_dir():
        print("No build tree at {}. Run ./gen_reference.sh first.".format(build),
              file=sys.stderr)
        return 2

    elfs = sorted(build.rglob("*.sig.elf"))
    if args.extensions:
        want = set(e.lower() for e in args.extensions)
        elfs = [e for e in elfs if e.parent.name.lower() in want]
    if args.limit:
        elfs = elfs[: args.limit]
    if not elfs:
        print("No matching ELFs.", file=sys.stderr)
        return 2

    outdir = HERE / "out"
    outdir.mkdir(exist_ok=True)

    counts = {"pass": 0, "fail": 0, "nosig": 0, "skip": 0}
    problems = []
    for i, elf in enumerate(elfs, 1):
        sail_sig = elf.with_suffix("")            # <name>.sig.elf -> <name>.sig
        if not sail_sig.exists():
            counts["skip"] += 1
            continue
        status, detail = run_one(elf, sail_sig, outdir, args.keep, args.timeout)
        counts[status] += 1
        if status in ("fail", "nosig"):
            problems.append("{}/{}: {}".format(
                elf.parent.name, elf.name[: -len(".sig.elf")], detail))
        print("\r[{}/{}] pass={} fail={} nosig={} skip={}".format(
            i, len(elfs), counts["pass"], counts["fail"],
            counts["nosig"], counts["skip"]), end="", flush=True)

    print("\n" + "=" * 62)
    print("  pass {}   fail {}   no-signature {}   skipped {}".format(
        counts["pass"], counts["fail"], counts["nosig"], counts["skip"]))
    print("=" * 62)
    for p in problems[:40]:
        print("  " + p)
    if len(problems) > 40:
        print("  ... and {} more".format(len(problems) - 40))
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
