#!/usr/bin/env python3
"""DoomV's privilege-mode switch (-priv=m|mu|msu), held to Sail.

Smaller machines than RVA23 -- machine mode alone, machine and user, the
three -- on the ISAs a small core starts with. For each, every rv64
riscv-test runs on Sail configured as that machine (sail_machine.py) and
on DoomV with the same -priv and -march, in strict lock-step. A test need
not pass on such a machine -- a supervisor test on a hart without S traps
its way to a failure -- but DoomV has to do exactly what Sail does, record
for record, to the test's end.

    python tools/verification/priv_lockstep.py
    python tools/verification/priv_lockstep.py --machines m:rv64i_zicsr --tests rv64ui-p-add
"""
import argparse
import concurrent.futures
import json
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "verification"))
import lockstep_sail  # noqa: E402
import sail_machine  # noqa: E402

run_suite = lockstep_sail.run_suite
WORK = ROOT / "build" / "priv-lockstep"
# Counters and their filter, which DoomV always has; Sail is given them too.
COMMON = "_zicntr_zihpm_smcntrpmf"
MACHINES = [
    ("m", "rv64i_zicsr_zifencei"),
    ("m", "rv64imac_zicsr_zifencei"),
    ("mu", "rv64imac_zicsr_zifencei"),
    ("mu", "rv64imafdc_zicsr_zifencei"),
    ("msu", "rv64imafdc_zicsr_zifencei"),
]


def one(priv, march, elf, work, timeout):
    work.mkdir(parents=True, exist_ok=True)
    config = work / "sail.json"
    config.write_text(json.dumps(sail_machine.config(priv, march + COMMON), indent=2))
    trace = work / "sail.log"
    r = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", run_suite.WSL_SAIL, "--config",
                        lockstep_sail.wsl(config), "--inst-limit", "200000"] + lockstep_sail.SAIL_FLAGS
                       + ["--trace-output", lockstep_sail.wsl(trace), lockstep_sail.wsl(elf)],
                       capture_output=True, text=True, timeout=timeout, env=run_suite._env())
    said = (r.stdout or "") + (r.stderr or "")
    if "invalid" in said:
        return "fail", "Sail rejects the configuration: " + said.strip()[-400:]
    if not trace.exists() or trace.stat().st_size == 0:
        return "skip", "Sail wrote no trace"
    syms = run_suite.elf_symbols(elf)
    d = subprocess.run([str(ROOT / "riscv_doom.exe"), "-ng", str(ROOT / "tools/doom/doombuild/DOOM1.WAD"), str(elf),
                        f"-march={march}", f"-priv={priv}", "-tohost={:x}".format(syms["tohost"]),
                        f"-lockstep={trace}", "-lockstep-strict", "-stopat=200000"],
                       cwd=work, capture_output=True, text=True, timeout=timeout)
    out = (d.stdout or "") + (d.stderr or "")
    if "MISMATCH" in out:
        return "fail", out[out.index("lockstep: MISMATCH"):].strip()[:2500]
    verdict = next((l for l in out.splitlines() if l.startswith("lockstep: ") and "skipped" not in l), "")
    if "matched" not in verdict and "all matching" not in verdict:
        return "fail", "no lock-step verdict: " + out.strip()[-400:]
    return "pass", verdict.removeprefix("lockstep: ")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--machines", help="priv:march,... (default: a set from m:rv64i to msu:rv64imafdc)")
    ap.add_argument("--tests", help="comma-separated test names (default: every rv64 riscv-test)")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()
    machines = [tuple(m.split(":")) for m in args.machines.split(",")] if args.machines else MACHINES
    tests = [lockstep_sail.TESTS / t for t in args.tests.split(",")] if args.tests else sorted(
        p for p in lockstep_sail.TESTS.iterdir()
        if re.match(r"rv64(u[imacfd]|mi|si)-p-", p.name) and p.is_file() and not p.suffix)
    shutil.rmtree(WORK, ignore_errors=True)
    failed = 0
    for priv, march in machines:
        jobs = {}
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            for t in tests:
                jobs[t.name] = pool.submit(one, priv, march, t, WORK / f"{priv}-{march}" / t.name, args.timeout)
            counts = {}
            for name, f in jobs.items():
                status, detail = f.result()
                counts[status] = counts.get(status, 0) + 1
                if status == "fail":
                    failed += 1
                    print(f"FAIL {priv} {march} {name}\n  " + detail.replace("\n", "\n  "), flush=True)
        print(f"{priv:4} {march}: " + "  ".join(f"{k} {v}" for k, v in sorted(counts.items())), flush=True)
    print("\nall pass" if not failed else f"\n{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
