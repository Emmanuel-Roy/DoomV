#!/usr/bin/env python3
"""Build the emulator with profile-guided optimization.

GCC optimizes better when it knows which branches the interpreter takes and
which calls are hot, and a profile of real runs tells it. Three steps:

  1. an instrumented build, into build/pgo/gen/
  2. training: the same two workloads bench.py measures, run to the same
     step counts, which write .gcda profiles into build/pgo/data/
  3. the final build with -fprofile-use, copied to riscv_doom.exe

Both builds go to the same path under build/pgo/gen/, because GCC 11 and later
name each profile after the *output* they were compiled for; building the two
stages to different paths leaves the final build looking for profiles that do
not exist (it says so, per file, as -Wmissing-profile).

--cxx/--cc build with another compiler, and --lto adds -flto. Neither is the
default: the installed GCC 8.1 cannot link this project with -flto at all, and
a newer GCC is not assumed to be present. See performance/README.md for what
each combination measured.

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


def make(out: Path, extra: str, log: Path, cxx=None, cc=None):
    rel = out.relative_to(ROOT).as_posix()
    cmd = ["make", "-B", f"-j{os.environ.get('JOBS', '4')}", f"OUT={rel}", f"CXXFLAGS={BASE} {extra}"]
    if cxx:
        cmd.append(f"CXX={cxx}")
    if cc:
        cmd.append(f"CC={cc}")
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
    ap.add_argument("--cxx", help="build with this C++ compiler instead of the Makefile's")
    ap.add_argument("--cc", help="...and this C compiler, for SoftFloat (defaults beside --cxx's gcc)")
    ap.add_argument("--lto", action="store_true", help="add -flto (needs a compiler whose LTO works: not GCC 8.1)")
    args = ap.parse_args()
    lto = " -flto=auto" if args.lto else ""

    shutil.rmtree(PGO, ignore_errors=True)
    GEN.mkdir(parents=True)
    DATA.mkdir(parents=True)
    # Both stages build to this one path: GCC 11 and later name each profile
    # after the output it was compiled for, so two different paths means the
    # final build looks for profiles that were never written.
    staged = GEN / "riscv_doom.exe"

    print("1/3 instrumented build", flush=True)
    # The same Windows-style profile directory the optimized build reads, so
    # the names the instrumented binary writes are the names GCC looks for.
    make(staged, f"-fprofile-generate -fprofile-dir={windows_path(DATA)}{lto}",
         PGO / "build-gen.log", args.cxx, args.cc)
    for dll in ROOT.glob("*.dll"):
        shutil.copy2(dll, GEN / dll.name)

    print("2/3 training", flush=True)
    env = dict(os.environ)
    if not args.cxx:
        # GCC 8.1's runtime writes each profile under the compile-time
        # directory of the object, and silently writes nothing when that path
        # is not one Windows understands. GCOV_PREFIX redirects it, stripping
        # every component so the files land side by side. A newer GCC names
        # them itself, relative to -fprofile-dir, and redirecting instead
        # produces names its -fprofile-use will not look for.
        env.update(GCOV_PREFIX=windows_path(DATA), GCOV_PREFIX_STRIP="64")
    for name, spec in bench.WORKLOADS.items():
        cmd = [str(staged)] + spec["args"]() + [f"-stopat={spec['steps']}"]
        with (PGO / f"train-{name}.log").open("wb") as f:
            r = subprocess.run(cmd, cwd=GEN, env=env, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
        print(f"    {name}: exit {r.returncode}", flush=True)
        if r.returncode != 0:
            raise RuntimeError(f"training run {name} failed; see {PGO / f'train-{name}.log'}")
    profiles = sorted(DATA.rglob("*.gcda"))
    if not profiles:
        raise RuntimeError(f"training wrote no profiles into {DATA}")
    print(f"    {len(profiles)} profiles", flush=True)

    print("3/3 optimized build -> riscv_doom.exe", flush=True)
    log = make(staged, f"-fprofile-use -fprofile-correction -fprofile-dir={windows_path(DATA)}{lto}",
               PGO / "build-use.log", args.cxx, args.cc)
    if "profile count data file not found" in log:
        raise RuntimeError(f"GCC found no profiles; the binary is not a PGO build. See {PGO / 'build-use.log'}")
    shutil.copy2(staged, ROOT / "riscv_doom.exe")
    print("done: riscv_doom.exe is a PGO build", flush=True)

    if args.bench:
        for workload in bench.WORKLOADS:
            subprocess.run([sys.executable, str(HERE / "bench.py"), workload, "--compare", str(args.baseline),
                            "--label", "PGO build"], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
