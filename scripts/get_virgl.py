#!/usr/bin/env python3
"""Fetch virglrenderer, which -gpu=virgl uses to run the guest's OpenGL on the host GPU.

DoomV loads it at run time, only for -gpu=virgl, so nothing else needs it:
a checkout without it builds and runs as before. It comes from MSYS2's
mingw64 repository -- the build of virglrenderer for Windows, with libepoxy,
which reaches OpenGL through WGL -- unpacked into build/virgl/. mingw64
rather than ucrt64 or clang64 because it uses MSVCRT, the C runtime DoomV's
toolchain and SDL2 use.

  python scripts/get_virgl.py            # fetch if not already there
  python scripts/get_virgl.py --force    # fetch again over an existing copy
"""
from __future__ import annotations

import argparse
import io
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "virgl"
REPO = "https://repo.msys2.org/mingw/mingw64/"

# Pinned: the versions -gpu=virgl was tested with.
PACKAGES = [
    "mingw-w64-x86_64-virglrenderer-1.3.0-1-any.pkg.tar.zst",
    "mingw-w64-x86_64-libepoxy-1.5.10-7-any.pkg.tar.zst",
    "mingw-w64-x86_64-libgcc-16.2.0-4-any.pkg.tar.zst",
    "mingw-w64-x86_64-libwinpthread-14.0.0.r426.g4564ee4b5-1-any.pkg.tar.zst",
]
LIBRARY = OUT / "bin" / "libvirglrenderer-1.dll"


def unpack(data: bytes):
    import zstandard
    raw = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(data)).read()
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for member in tar.getmembers():
            # mingw64/bin/*.dll and mingw64/include/virgl/*.h are all that is used.
            name = member.name
            if not name.startswith("mingw64/") or not member.isfile():
                continue
            rel = name[len("mingw64/"):]
            if not (rel.startswith("bin/") and rel.endswith(".dll")) and not rel.startswith("include/"):
                continue
            target = OUT / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="fetch even if already unpacked")
    args = ap.parse_args()
    if LIBRARY.exists() and not args.force:
        print(f"already present: {LIBRARY.relative_to(ROOT)}")
        return 0
    try:
        import zstandard  # noqa: F401
    except ImportError:
        sys.exit("needs the zstandard module: python -m pip install zstandard")
    OUT.mkdir(parents=True, exist_ok=True)
    for package in PACKAGES:
        print(f"downloading {package}", flush=True)
        with urllib.request.urlopen(REPO + package) as response:
            unpack(response.read())
    if not LIBRARY.exists():
        sys.exit(f"unpacked, but {LIBRARY} is not there")
    print(f"virglrenderer is in {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
