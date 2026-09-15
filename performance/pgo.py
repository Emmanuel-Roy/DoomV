#!/usr/bin/env python3
"""Build the emulator with profile-guided optimization.

GCC optimizes better when it knows which branches the interpreter takes and
which calls are hot, and a profile of real runs tells it. Three steps:

  1. an instrumented build, into build/pgo/gen/
  2. training: the same two workloads bench.py measures, run to the same
     step counts, which write per-source .gcda profiles into build/pgo/data/
  3. the final build with -fprofile-use, written to riscv_doom.exe

It changes how fast the code runs, not what it does -- the sources are the
same, and bench.py's crash.log comparison is the check. `make` stays the plain
build: this one takes minutes, because it runs the emulator in the middle.

  python performance/pgo.py              # build riscv_doom.exe with PGO
  python performance/pgo.py --bench      # ...then record it against the baseline

Two things this toolchain (MinGW GCC 8.1) needs, found the hard way:
  * The profiling runtime cannot write to a Git Bash path like /z/Code/...,
    and fails silently. GCOV_PREFIX redirects its output to a Windows path.
  * -fprofile-use has to be given the Windows path too, or GCC finds no
    profiles and builds an ordinary binary without saying so.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import bench  # noqa: E402  (the workloads)

PGO = ROOT / "build" / "pgo"
GEN = PGO / "gen"
DATA = PGO / "data"

# The Makefile's flags without CXXFLAGS' optimization choices changed -- the
# profile flags are added to them, nothing is taken away.
BASE = ("-std=c++2a -O3 -pthread -frounding-math -static-libgcc -static-libstdc++ "
        "-Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic")


def make(out: Path, extra: str, log: Path):
    rel = out.relative_to(ROOT).as_posix()
    cmd = ["make", "-B", f"-j{os.environ.get('JOBS', '4')}", f"OUT={rel}", f"CXXFLAGS={BASE} {extra}"]
    with log.open("wb") as f:
        r = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError(f"build failed; see {log}")
    return log.read_text(errors="replace")


def windows_path(p: Path) -> str:
    return str(p.resolve()).replace("\\", "/")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", action="store_true", help="afterwards, record bench.py runs against the baseline")
    ap.add_argument("--baseline", type=Path, default=ROOT / "build" / "perf-baseline" / "riscv_doom.exe")
    args = ap.parse_args()

    shutil.rmtree(PGO, ignore_errors=True)
    GEN.mkdir(parents=True)
    DATA.mkdir(parents=True)

    print("1/3 instrumented build", flush=True)
    # The same Windows-style profile directory the optimized build reads, so
    # the names the instrumented binary writes are the names GCC looks for.
    make(GEN / "riscv_doom.exe", f"-fprofile-generate -fprofile-dir={windows_path(DATA)}", PGO / "build-gen.log")
    for dll in ROOT.glob("*.dll"):
        shutil.copy2(dll, GEN / dll.name)

    # The instrumented binary would write each profile under the object's
    # compile-time directory; stripping every component of that and prefixing
    # the data directory puts them all side by side, one per source file.
    print("2/3 training", flush=True)
    env = dict(os.environ, GCOV_PREFIX=windows_path(DATA), GCOV_PREFIX_STRIP="64")
    for name, spec in bench.WORKLOADS.items():
        cmd = [str(GEN / "riscv_doom.exe")] + spec["args"]() + [f"-stopat={spec['steps']}"]
        with (PGO / f"train-{name}.log").open("wb") as f:
            r = subprocess.run(cmd, cwd=GEN, env=env, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
        print(f"    {name}: exit {r.returncode}", flush=True)
        if r.returncode != 0:
            raise RuntimeError(f"training run {name} failed; see {PGO / f'train-{name}.log'}")
    profiles = sorted(DATA.glob("*.gcda"))
    if not profiles:
        raise RuntimeError(f"training wrote no profiles into {DATA}")
    print(f"    {len(profiles)} profiles", flush=True)

    print("3/3 optimized build -> riscv_doom.exe", flush=True)
    log = make(ROOT / "riscv_doom.exe",
               f"-fprofile-use -fprofile-correction -fprofile-dir={windows_path(DATA)}",
               PGO / "build-use.log")
    if "profile count data file not found" in log:
        raise RuntimeError(f"GCC found no profiles; the binary is not a PGO build. See {PGO / 'build-use.log'}")
    print("done: riscv_doom.exe is a PGO build", flush=True)

    if args.bench:
        for workload in bench.WORKLOADS:
            subprocess.run([sys.executable, str(HERE / "bench.py"), workload, "--compare", str(args.baseline),
                            "--label", "PGO build"], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
