#!/usr/bin/env python3
"""Compare how the guest's RAM is allocated, by throughput.

GuestRam can back the guest's memory three ways (see src/guest_ram.hpp), and
which one is fastest is a question about this workload rather than about
allocators in general. This runs each backend over each workload several
times and reports the spread, because the differences in question are a
percent or two and a single run cannot tell one of those from noise.

It does not write to RUNS.md: that table is for changes to the emulator, and
eighteen rows of the same build configured differently would bury it. The
crash.log hash is still checked on every run -- a backend that changed
behaviour would be a bug, not a result.

  python performance/ram_backends.py                  # every backend, both workloads
  python performance/ram_backends.py --repeat 5
  python performance/ram_backends.py --workload doom
  python performance/ram_backends.py --ram 4G         # ...at a larger guest size
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402

BACKENDS = ("vector", "pages", "hugepages")


def sha(path: Path) -> str:
    h = hashlib.sha256()
    if path.exists():
        h.update(path.read_bytes())
    return h.hexdigest()[:12]


def one_run(exe: Path, workload: str, backend: str, ram: str | None) -> tuple[float, str]:
    spec = bench.WORKLOADS[workload]
    cmd = [str(exe.resolve())] + spec["args"]() + [f"-stopat={spec['steps']}"]
    if ram:
        cmd.insert(1, f"-ram={ram}")
    env = dict(os.environ, DOOMV_RAM_BACKEND=backend)
    # In a clean directory, exactly as bench.py does it, and for the same
    # reason: the emulator attaches `drives` and `shared` from the working
    # directory by default, so running in the repo root hands the guest this
    # checkout's real folders -- a different machine from the one bench.py
    # measures, and one whose contents can change between runs. crash.log
    # lands here too, which is what gets hashed.
    work = ROOT / "build" / "ram-backends" / f"{workload}-{backend}"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    with (work / "console.log").open("wb") as console:
        proc = subprocess.run(cmd, cwd=work, env=env, stdin=subprocess.DEVNULL,
                              stdout=console, stderr=subprocess.STDOUT)
    seconds = time.perf_counter() - start
    if proc.returncode != 0:
        raise RuntimeError(f"{workload}/{backend} exited {proc.returncode}; see {work / 'console.log'}")
    return spec["steps"] / seconds / 1e6, sha(work / "crash.log")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, default=ROOT / "riscv_doom.exe")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--workload", choices=sorted(bench.WORKLOADS), action="append")
    ap.add_argument("--backend", choices=BACKENDS, action="append")
    ap.add_argument("--ram", help="pass -ram=<size> to every run")
    args = ap.parse_args()

    workloads = args.workload or sorted(w for w, spec in bench.WORKLOADS.items()
                                        if not spec.get("optional"))
    backends = args.backend or list(BACKENDS)

    print(f"{args.exe.name}, {args.repeat} runs each"
          + (f", -ram={args.ram}" if args.ram else "") + "\n")
    for workload in workloads:
        steps = bench.WORKLOADS[workload]["steps"]
        print(f"{workload} ({steps / 1e6:.0f}M steps)")
        print(f"  {'backend':<10} {'median':>8} {'min':>8} {'max':>8}   crash.log")
        baseline = None
        for backend in backends:
            mips, hashes = [], set()
            for _ in range(args.repeat):
                m, h = one_run(args.exe, workload, backend, args.ram)
                mips.append(m)
                hashes.add(h)
            median = statistics.median(mips)
            if baseline is None:
                baseline = median
            note = "" if len(hashes) == 1 else "  DIFFERING HASHES"
            rel = f"  {median / baseline:.3f}x" if baseline else ""
            print(f"  {backend:<10} {median:7.2f}  {min(mips):7.2f}  {max(mips):7.2f}   "
                  f"{sorted(hashes)[0]}{note}{rel}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
