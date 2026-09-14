#!/usr/bin/env python3
"""Lock-step DoomV against Sail, the golden reference, one test at a time.

For every riscv-tests ELF Sail passes, Sail writes its full trace (every
instruction, register, CSR, store, exception and interrupt), and DoomV runs
the same ELF with -lockstep against that trace. DoomV halts at the first
record that differs from Sail's, so a failure here names the exact
instruction and field where DoomV and the reference parted ways.

    python tools/verification/lockstep_sail.py              # every rv64 test
    python tools/verification/lockstep_sail.py rv64ui-p-add rv64mi-p-illegal
    python tools/verification/lockstep_sail.py --jobs 8 --keep
"""
import argparse
import concurrent.futures
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SUITES = ROOT / "tools" / "verification" / "tests" / "suites"
sys.path.insert(0, str(SUITES))
import run_suite  # noqa: E402  (the suite runner's march, Sail paths and ELF symbol reader)

TESTS = SUITES / "riscv-tests"
WORK = ROOT / "build" / "lockstep-sail"
SAIL_FLAGS = ["--trace-instr", "--trace-gpr", "--trace-fpr", "--trace-vreg", "--trace-csr",
              "--trace-mem", "--trace-exception", "--trace-interrupt"]


# Sail runs with the suites' configuration exactly as it is. DoomV is the one
# that has to be that hart: the reference is not adjusted to fit it.
SAIL_CONFIG = ROOT / "tools" / "verification" / "simulators" / "sail" / "rva23s64.json"


def wsl(path: pathlib.Path) -> str:
    s = str(path.resolve()).replace("\\", "/")
    return "/mnt/" + s[0].lower() + s[2:]


# Hand-written tests for what the riscv-tests leave alone -- the clock, the
# counters and the waits. They check nothing themselves: the lock-step is the
# check.
LOCKSTEP_TESTS = ROOT / "tools" / "verification" / "tests" / "lockstep"
LOCKSTEP_ELFS = ROOT / "build" / "lockstep-elf"


def build_lockstep_tests() -> dict:
    LOCKSTEP_ELFS.mkdir(parents=True, exist_ok=True)
    built = {}
    for source in sorted(LOCKSTEP_TESTS.glob("*.S")):
        elf = LOCKSTEP_ELFS / source.stem
        subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", "riscv64-unknown-elf-gcc",
                        "-march=rv64ima_zicsr", "-mabi=lp64", "-nostdlib", "-static",
                        "-Wl,-N", "-Wl,-Ttext=0x80000000", "-Wl,--no-relax",
                        "-o", wsl(elf), wsl(source)], check=True, env=run_suite._env())
        built[elf.name] = elf
    return built


def one(elf: pathlib.Path, config: pathlib.Path, keep: bool, timeout: int):
    work = WORK / elf.name
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    trace = work / "sail.log"

    sail = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", run_suite.WSL_SAIL, "--config", wsl(config)]
                          + SAIL_FLAGS + ["--trace-output", wsl(trace), wsl(elf)],
                          capture_output=True, text=True, timeout=timeout, env=run_suite._env())
    if "SUCCESS" not in (sail.stdout or "") + (sail.stderr or ""):
        shutil.rmtree(work, ignore_errors=True)
        return elf.name, "skip", "Sail does not pass it"

    syms = run_suite.elf_symbols(elf)
    cmd = [str(ROOT / "riscv_doom.exe"), "-ng", str(ROOT / "tools" / "doom" / "doombuild" / "DOOM1.WAD"), str(elf),
           "-march=" + run_suite.SUITE_MARCH, "-tohost={:x}".format(syms["tohost"]),
           "-lockstep=" + str(trace), "-lockstep-strict", "-stopat=100000000"]
    try:
        r = subprocess.run(cmd, cwd=work, capture_output=True, text=True, timeout=timeout)
        out = (r.stdout or "") + (r.stderr or "")
        code = r.returncode
    except subprocess.TimeoutExpired:
        return elf.name, "fail", "DoomV timed out"

    if "MISMATCH" in out:
        at = out.index("lockstep: MISMATCH")
        return elf.name, "fail", out[at:].strip()
    tohost = (work / "tohost.log").read_text().strip() if (work / "tohost.log").exists() else "?"
    summary = next((l for l in out.splitlines() if l.startswith("lockstep: ")), "no lockstep summary")
    if code != 0 or tohost != "1":
        return elf.name, "fail", f"exit {code}, tohost {tohost}: {summary}"
    if not keep:
        shutil.rmtree(work, ignore_errors=True)
    return elf.name, "pass", summary


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tests", nargs="*", help="test names; default: every rv64 test")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--keep", action="store_true", help="keep traces of passing tests")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    own = build_lockstep_tests()
    if args.tests:
        elfs = [own.get(t, TESTS / t) for t in args.tests]
    else:
        elfs = sorted(p for p in TESTS.iterdir()
                      if re.match(r"rv64[a-z]+-[pv]-", p.name) and p.is_file() and not p.suffix)
        elfs += list(own.values())
    WORK.mkdir(parents=True, exist_ok=True)
    config = SAIL_CONFIG

    results = {"pass": [], "fail": [], "skip": []}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for name, status, detail in pool.map(lambda e: one(e, config, args.keep, args.timeout), elfs):
            results[status].append((name, detail))
            if status == "fail":
                print(f"FAIL {name}\n  " + detail.replace("\n", "\n  "), flush=True)
            else:
                print(f"{status:4} {name}: {detail}", flush=True)

    print(f"\npass {len(results['pass'])}  fail {len(results['fail'])}  skip {len(results['skip'])}")
    return 1 if results["fail"] else 0


if __name__ == "__main__":
    sys.exit(main())
