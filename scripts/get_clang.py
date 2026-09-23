#!/usr/bin/env python3
"""Fetch the Clang the Makefile builds with by default.

The Makefile prefers a clang unpacked under build/toolchains/ and falls back
to GCC when there is none, so this script is what turns a GCC-only checkout
into a Clang one. Nothing else depends on it having been run: `make` and
scripts/verify.py both work either way, and the two builds pass the same
tests. See performance/README.md for why Clang is the default.

llvm-mingw rather than a plain LLVM: this project links MinGW import
libraries (src/lib) and a MinGW-built SDL2, and llvm-mingw is the Clang that
targets that ABI. The MSVCRT build rather than the UCRT one, to match the C
runtime the installed GCC and the bundled SDL2 already use.

  python scripts/get_clang.py            # fetch if not already there
  python scripts/get_clang.py --force    # fetch again over an existing copy
"""
from __future__ import annotations

import argparse
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLCHAINS = ROOT / "build" / "toolchains"

# Pinned, because it is the version performance/README.md's numbers were
# measured with and the one both test suites have been run against.
RELEASE = "20260908"
ARCHIVE = f"llvm-mingw-{RELEASE}-msvcrt-x86_64.zip"
URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{RELEASE}/{ARCHIVE}"


def existing() -> Path | None:
    for clang in sorted(TOOLCHAINS.glob("llvm-mingw-*/bin/clang++.exe")):
        return clang
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="fetch even if a copy is already unpacked")
    args = ap.parse_args()

    found = existing()
    if found and not args.force:
        print(f"already present: {found.relative_to(ROOT)}")
        return 0

    TOOLCHAINS.mkdir(parents=True, exist_ok=True)
    archive = TOOLCHAINS / ARCHIVE
    print(f"downloading {URL}", flush=True)
    # To a temporary name first, so an interrupted download cannot leave
    # something that looks like a complete archive behind.
    partial = archive.with_suffix(".part")
    with urllib.request.urlopen(URL) as response, partial.open("wb") as out:
        shutil.copyfileobj(response, out)
    partial.replace(archive)
    print(f"  {archive.stat().st_size / 1e6:.0f} MB", flush=True)

    print("unpacking", flush=True)
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(TOOLCHAINS)
    archive.unlink()

    clang = existing()
    if clang is None:
        raise RuntimeError(f"unpacked {ARCHIVE} but found no clang++.exe under {TOOLCHAINS}")
    # The zip does not carry the executable bit, which is irrelevant on
    # Windows but matters when make is driven from Git Bash or MSYS.
    for exe in clang.parent.glob("*.exe"):
        exe.chmod(exe.stat().st_mode | 0o111)
    print(f"done: make will now build with {clang.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
