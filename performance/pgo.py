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

Clang and LTO are both the default, because that is the fastest combination
measured and the one the recorded numbers use -- 1.358x on an Ubuntu boot over
the plain build. --cxx/--cc pick another compiler and --no-lto turns LTO off,
which is what the installed GCC 8.1 needs: its LTO plugin cannot link this
project at all. See performance/README.md for what each combination measured.

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


def default_compiler() -> tuple[str, str]:
    """The (C++, C) compilers `make` picks when none is named on its line.

    This mirrors the Makefile's own selection, and has to: with --cxx absent,
    make builds with whatever it prefers, so anything here that assumed GCC
    would be preparing profiles for a compiler that is not the one building.
    """
    for clang in sorted(ROOT.glob("build/toolchains/llvm-mingw-*/bin/clang++.exe")):
        return str(clang), str(clang.with_name("clang.exe"))
    if shutil.which("clang++"):
        return "clang++", "clang"
    return "g++", "gcc"


def is_clang(cxx) -> bool:
    """Whether --cxx names a clang, which profiles differently from GCC."""
    if not cxx:
        return False
    try:
        out = subprocess.run([cxx, "--version"], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "clang" in out.lower()


def merge_profraw(cxx, data: Path) -> Path:
    """Clang's .profraw files into the single .profdata -fprofile-use wants.

    GCC leaves one .gcda per object and reads them straight back from
    -fprofile-dir. Clang instead writes a .profraw per *run* of the
    instrumented binary, and they have to be merged by llvm-profdata -- which
    ships beside the compiler, so it is found there rather than on PATH.
    """
    raw = sorted(data.rglob("*.profraw"))
    if not raw:
        raise RuntimeError(f"training wrote no .profraw into {data}")
    profdata = data / "merged.profdata"
    tool = Path(cxx).resolve().parent / "llvm-profdata.exe"
    if not tool.exists():
        tool = Path("llvm-profdata")
    r = subprocess.run([str(tool), "merge", f"-output={windows_path(profdata)}"]
                       + [windows_path(f) for f in raw], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"llvm-profdata merge failed: {r.stderr.strip()}")
    print(f"    {len(raw)} .profraw merged", flush=True)
    return profdata


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", action="store_true", help="afterwards, record bench.py runs against the baseline")
    ap.add_argument("--baseline", type=Path, default=ROOT / "build" / "perf-baseline" / "riscv_doom.exe")
    ap.add_argument("--cxx", help="build with this C++ compiler instead of the Makefile's")
    ap.add_argument("--cc", help="...and this C compiler, for SoftFloat (defaults beside --cxx's gcc)")
    # LTO on by default: with Clang it works, is measured faster in every
    # configuration tried, and is what the recorded numbers use. --no-lto is
    # there for GCC, whose 8.1 cannot link this project with -flto at all.
    ap.add_argument("--lto", action=argparse.BooleanOptionalAction, default=True,
                    help="link-time optimization (default: on; --no-lto for GCC 8.1, which cannot)")
    args = ap.parse_args()
    # An explicit --cxx wins; otherwise resolve what make would use, so the
    # profile flow below matches the compiler that actually does the building.
    explicit_cxx = args.cxx is not None
    if not explicit_cxx:
        args.cxx, args.cc = default_compiler()
    elif args.cc is None:
        args.cc = args.cxx.replace("clang++", "clang").replace("g++", "gcc")
    clang = is_clang(args.cxx)
    print(f"compiler: {args.cxx}", flush=True)
    # ThinLTO is clang's scalable form and the one that actually links this
    # project; GCC's -flto=auto is the equivalent spelling there.
    lto = ""
    if args.lto:
        lto = " -flto=thin" if clang else " -flto=auto"

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
    # Clang takes the directory as part of the flag and writes .profraw into
    # it at run time; GCC takes -fprofile-dir and writes .gcda at compile time.
    gen_flags = (f"-fprofile-generate={windows_path(DATA)}" if clang
                 else f"-fprofile-generate -fprofile-dir={windows_path(DATA)}")
    make(staged, f"{gen_flags}{lto}", PGO / "build-gen.log", args.cxx, args.cc)
    for dll in ROOT.glob("*.dll"):
        shutil.copy2(dll, GEN / dll.name)

    print("2/3 training", flush=True)
    env = dict(os.environ)
    if not explicit_cxx and not clang:  # the bundled GCC 8.1; nothing else needs this
        # GCC 8.1's runtime writes each profile under the compile-time
        # directory of the object, and silently writes nothing when that path
        # is not one Windows understands. GCOV_PREFIX redirects it, stripping
        # every component so the files land side by side. A newer GCC names
        # them itself, relative to -fprofile-dir, and redirecting instead
        # produces names its -fprofile-use will not look for.
        env.update(GCOV_PREFIX=windows_path(DATA), GCOV_PREFIX_STRIP="64")
    # Optional workloads are skipped: training must work on a checkout that
    # has only what `scripts/build.py all` produces, and ubuntu.img is not
    # that. A profile from the two core workloads is what the recorded PGO
    # numbers were measured with anyway.
    for name, spec in bench.WORKLOADS.items():
        if spec.get("optional"):
            continue
        cmd = [str(staged)] + spec["args"]() + [f"-stopat={spec['steps']}"]
        with (PGO / f"train-{name}.log").open("wb") as f:
            r = subprocess.run(cmd, cwd=GEN, env=env, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
        print(f"    {name}: exit {r.returncode}", flush=True)
        if r.returncode != 0:
            raise RuntimeError(f"training run {name} failed; see {PGO / f'train-{name}.log'}")
    if clang:
        use_flags = f"-fprofile-use={windows_path(merge_profraw(args.cxx, DATA))}"
    else:
        profiles = sorted(DATA.rglob("*.gcda"))
        if not profiles:
            raise RuntimeError(f"training wrote no profiles into {DATA}")
        print(f"    {len(profiles)} profiles", flush=True)
        use_flags = f"-fprofile-use -fprofile-correction -fprofile-dir={windows_path(DATA)}"

    print("3/3 optimized build -> riscv_doom.exe", flush=True)
    log = make(staged, f"{use_flags}{lto}", PGO / "build-use.log", args.cxx, args.cc)
    if "profile count data file not found" in log:
        raise RuntimeError(f"GCC found no profiles; the binary is not a PGO build. See {PGO / 'build-use.log'}")
    if clang and "profile data may be out of date" in log:
        print("    warning: clang reports stale profile data for some functions", flush=True)
    shutil.copy2(staged, ROOT / "riscv_doom.exe")
    print("done: riscv_doom.exe is a PGO build", flush=True)

    if args.bench:
        for workload in bench.WORKLOADS:
            subprocess.run([sys.executable, str(HERE / "bench.py"), workload, "--compare", str(args.baseline),
                            "--label", "PGO build"], cwd=ROOT, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
