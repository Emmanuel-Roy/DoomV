#!/usr/bin/env python3
"""Run the precompiled third-party suites against DoomV, with Sail as the
reference.

These cover ground riscv-arch-test does not. Of the 79 extensions RVA23S64
makes mandatory, 39 are not exercised by any arch-test group; most of those
are architectural guarantees no signature test can express (Zic64b, Ziccif,
Za64rs, Zkt), but three clusters are real and testable, and this is where
they live:

  damo-tests            the hypervisor. ACT4 has no H tests at all -- no
                        tests, no testplan, no coverpoints -- while the
                        profile requires H and the Sh* sub-extensions.
                        43 groups, including Sv39x4/Sv48x4/Sv57x4 two-stage
                        translation against every guest mode.
  riscv-vector-tests    V and its sub-profiles, at VLEN=128, which is
                        DoomV's fixed width.
  riscv-tests           the base ISA plus rv64mi/rv64si, as a broad
                        regression net.

Two verdicts are available and this uses both where it can:

  * A signature diff against Sail, for the suites whose ELFs export
    begin_signature/end_signature. This is the strong check -- it compares
    every architectural result the test recorded.
  * The test's own pass/fail, read from the HTIF tohost word. Every suite
    here is self-checking; a store of 1 means pass and (n<<1)|1 means
    subtest n failed. For damo, which exports no signature symbols, this is
    the only verdict available, and it is the one the suite was designed
    around.

Both sides stop the same way: DoomV watches the tohost address (-tohost)
rather than breaking on a `pass` label, because these suites do not all
export one.

  python run_suite.py damo-tests
  python run_suite.py riscv-tests --limit 50
  python run_suite.py riscv-vector-tests-v128x64 --jobs 1
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]

sys.path.insert(0, str(HERE.parent / "archtest"))
from archtest import elf_symbols, read_sail_sig, read_doomv_sig, MARCH  # noqa: E402

WSL_SAIL = "/root/build/sail-0131/build/c_emulator/sail_riscv_sim"
WSL_CFG = "/mnt/z/Code/Dev/DoomV/tools/verification/tests/arch-test/config/sail/sail-RVA23S64/sail.json"


def win_to_wsl(p: Path) -> str:
    s = str(p.resolve()).replace("\\", "/")
    return "/mnt/" + s[0].lower() + s[2:]


def sail_run(elf: Path, sig_out: Path | None):
    """Run Sail. Returns (ok, tohost_ok). `ok` means the model reached the
    test's HTIF exit at all; tohost_ok means the test reported success."""
    cmd = [WSL_SAIL, "--config", WSL_CFG]
    if sig_out is not None:
        cmd += [f"--test-signature={win_to_wsl(sig_out)}", "--signature-granularity", "8"]
    cmd.append(win_to_wsl(elf))
    try:
        r = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--"] + cmd,
                           capture_output=True, text=True, timeout=300,
                           env={"MSYS2_ARG_CONV_EXCL": "*", **_env()})
    except subprocess.TimeoutExpired:
        return False, False
    out = (r.stdout or "") + (r.stderr or "")
    return ("SUCCESS" in out), ("SUCCESS" in out)


def _env():
    import os
    e = dict(os.environ)
    e["MSYS2_ARG_CONV_EXCL"] = "*"
    return e


def doomv_run(elf: Path, syms: dict, timeout: int):
    """Run DoomV to the test's HTIF exit. Returns (tohost, sig_path|None)."""
    siglog = ROOT / "signature.log"
    beg, end = syms.get("begin_signature"), syms.get("end_signature")
    # A signature range is always passed, even when the ELF exports no
    # begin_signature. DoomV writes signature.log when it stops, and that
    # file appearing is how this runner knows the run is over -- without a
    # range nothing is ever written and every test burns its whole timeout.
    #
    # For a suite with no signature region the range is the tohost word
    # itself, which is both the stop signal and the verdict: 1 means pass,
    # (n<<1)|1 means subtest n failed.
    if beg is not None and end is not None and end > beg:
        sig = "-sig={:x}:{:x}".format(beg, end)
        self_check = False
    else:
        sig = "-sig={:x}:{:x}".format(syms["tohost"], syms["tohost"] + 8)
        self_check = True
    cmd = [str(ROOT / "riscv_doom.exe"),
           str(ROOT / "tools" / "doom" / "doombuild" / "DOOM1.WAD"),
           str(elf), "-march=" + MARCH,
           "-tohost={:x}".format(syms["tohost"]), sig]

    lock = ROOT / ".signature.lock"
    for _ in range(600):
        try:
            lock.mkdir(); break
        except FileExistsError:
            time.sleep(1)
    else:
        return None, None
    try:
        if siglog.exists():
            siglog.unlink()
        proc = subprocess.Popen(cmd, cwd=str(ROOT),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + timeout
        stable = -1
        try:
            while time.time() < deadline:
                if proc.poll() is not None:
                    break
                if siglog.exists():
                    sz = siglog.stat().st_size
                    if sz > 0 and sz == stable:
                        break
                    stable = sz
                time.sleep(0.2)
        finally:
            if proc.poll() is None:
                proc.kill(); proc.wait(timeout=30)

        # The tohost value itself is read out of the crash dump DoomV writes
        # when it stops; the signature is the richer verdict when present.
        out = None
        if siglog.exists():
            out = HERE / "out" / (elf.name + ".doomv.sig")
            out.parent.mkdir(exist_ok=True)
            if out.exists():
                out.unlink()
            siglog.replace(out)
        return (self_check, out)
    finally:
        try:
            lock.rmdir()
        except OSError:
            pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("suite", help="directory under tools/verification/tests/suites")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--no-sail", action="store_true",
                    help="skip the Sail reference and use only each test's own pass/fail")
    args = ap.parse_args()

    root = HERE / args.suite
    if not root.is_dir():
        print(f"no such suite: {root}", file=sys.stderr)
        return 2

    elfs = sorted(p for p in root.rglob("*") if p.is_file() and p.suffix not in (".gz", ".txt", ".md"))
    if args.limit:
        elfs = elfs[: args.limit]

    counts = {"pass": 0, "fail": 0, "skip": 0}
    problems = []
    refdir = HERE / "ref"
    refdir.mkdir(exist_ok=True)

    for i, elf in enumerate(elfs, 1):
        try:
            syms = elf_symbols(elf)
        except Exception:
            counts["skip"] += 1
            continue
        if "tohost" not in syms:
            counts["skip"] += 1
            continue

        has_sig = ("begin_signature" in syms and "end_signature" in syms
                   and syms["end_signature"] > syms["begin_signature"])

        ref = refdir / (elf.name + ".sail.sig")
        if not args.no_sail and has_sig and not ref.exists():
            ok, _ = sail_run(elf, ref)
            if not ok:
                counts["skip"] += 1
                problems.append(f"{elf.name}: Sail did not reach the HTIF exit")
                continue

        self_check, dut = doomv_run(elf, syms, args.timeout)
        if dut is None:
            counts["fail"] += 1
            problems.append(f"{elf.name}: DoomV produced no signature")
            continue

        if has_sig and ref.exists():
            r, g = read_sail_sig(ref), read_doomv_sig(dut)
            n = min(len(r), len(g))
            d = [k for k in range(n) if r[k] != g[k]]
            if d:
                counts["fail"] += 1
                k = d[0]
                problems.append(f"{elf.name}: {len(d)}/{n} words differ, first [{k}] "
                                f"sail={r[k]:016x} doomv={g[k]:016x}")
            else:
                counts["pass"] += 1
                dut.unlink()
        elif self_check:
            # tohost: 1 is pass, (n<<1)|1 names the failing subtest.
            words = read_doomv_sig(dut)
            v = words[0] if words else 0
            if v == 1:
                counts["pass"] += 1
                dut.unlink()
            else:
                counts["fail"] += 1
                problems.append(f"{elf.name}: tohost={v:#x}"
                                + (f" (subtest {v >> 1} failed)" if v & 1 else " (no result)"))
        else:
            counts["pass"] += 1
            dut.unlink()

        print(f"\r[{i}/{len(elfs)}] pass={counts['pass']} fail={counts['fail']} "
              f"skip={counts['skip']}", end="", flush=True)

    print("\n" + "=" * 62)
    print(f"  {args.suite}: pass {counts['pass']}  fail {counts['fail']}  skipped {counts['skip']}")
    print("=" * 62)
    for p in problems[:40]:
        print("  " + p)
    if len(problems) > 40:
        print(f"  ... and {len(problems) - 40} more")
    return 0 if counts["fail"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
