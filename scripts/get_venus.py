#!/usr/bin/env python3
"""Build the virglrenderer that -gpu loads: OpenGL and Vulkan in the guest, on the host GPU.

There is no prebuilt one. Venus on Windows comes from winq-emu's fork of
virglrenderer (MIT), and DoomV runs it in a mode the fork does not have --
synchronous, with no threads of its own, so the guest sees the same thing at
the same instruction on every run. That mode is tools/venus/doomv-sync.patch,
applied here to the fork at a pinned commit.

Everything is fetched and built under build/venus/, with this repository's
own Clang (scripts/get_clang.py): the fork's source, the Khronos Vulkan
headers it needs (newer than any MSYS2 package), MSYS2's libepoxy, EGL
headers and Vulkan loader import library, and meson and ninja in a private
Python environment. The result, with the DLLs it needs, is build/venus/bin/.
About two minutes.

  python scripts/get_venus.py            # build if not already there
  python scripts/get_venus.py --force    # build again
"""
from __future__ import annotations

import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "venus"
LIBRARY = OUT / "bin" / "libvirglrenderer-1.dll"
PATCH = ROOT / "tools" / "venus" / "doomv-sync.patch"

FORK = "https://github.com/cmspam/winq-emu-virglrenderer"
FORK_COMMIT = "e80354b8f5a0dedc62374b905a181874682659cc"   # winq-emu alpha 10, on virglrenderer 1.3.0
HEADERS = "https://github.com/KhronosGroup/Vulkan-Headers"
HEADERS_COMMIT = "c46850864f4661461b0f6cb9922c058ffea4915e"  # VK_HEADER_VERSION 365
MSYS2 = "https://repo.msys2.org/mingw/mingw64/"
PACKAGES = [   # mingw64: MSVCRT, as DoomV is
    "mingw-w64-x86_64-libepoxy-1.5.10-7-any.pkg.tar.zst",
    "mingw-w64-x86_64-egl-headers-1.5.r284.3ae2b7c-1-any.pkg.tar.zst",
    "mingw-w64-x86_64-vulkan-loader-1.4.317-2-any.pkg.tar.zst",
    "mingw-w64-x86_64-libwinpthread-14.0.0.r426.g4564ee4b5-1-any.pkg.tar.zst",
]


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def checkout(url: str, commit: str, dest: Path):
    """The tree at `commit`, exactly, fetched alone."""
    if not (dest / ".git").exists():
        dest.mkdir(parents=True, exist_ok=True)
        run(["git", "-C", dest, "init", "-q"])
        run(["git", "-C", dest, "remote", "add", "origin", url])
    run(["git", "-C", dest, "fetch", "-q", "--depth", "1", "origin", commit])
    run(["git", "-C", dest, "checkout", "-q", "-f", "FETCH_HEAD"])


def unpack(package: str, sysroot: Path):
    import zstandard
    with urllib.request.urlopen(MSYS2 + package) as response:
        raw = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(response.read())).read()
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for member in tar.getmembers():
            if member.isfile() and member.name.startswith("mingw64/"):
                target = sysroot / member.name[len("mingw64/"):]
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst)


def toolchain() -> Path:
    for clang in sorted((ROOT / "build" / "toolchains").glob("llvm-mingw-*/bin/clang.exe")):
        return clang.parent
    run([sys.executable, ROOT / "scripts" / "get_clang.py"])
    return toolchain()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="build again over an existing one")
    args = ap.parse_args()
    if LIBRARY.exists() and not args.force:
        print(f"already present: {LIBRARY.relative_to(ROOT)}")
        return 0
    try:
        import zstandard  # noqa: F401
    except ImportError:
        sys.exit("needs the zstandard module: python -m pip install zstandard")

    src, headers, sysroot, tools = OUT / "src", OUT / "vk-headers", OUT / "sysroot", OUT / "tools"
    checkout(FORK, FORK_COMMIT, src)
    run(["git", "-C", src, "apply", "--whitespace=nowarn", PATCH])
    checkout(HEADERS, HEADERS_COMMIT, headers)

    for package in PACKAGES:
        print(f"unpacking {package}", flush=True)
        unpack(package, sysroot)
    for sub in ("vulkan", "vk_video"):   # the fork's protocol needs newer headers than MSYS2 has
        shutil.rmtree(sysroot / "include" / sub, ignore_errors=True)
        shutil.copytree(headers / "include" / sub, sysroot / "include" / sub)

    if not (tools / "Scripts" / "meson.exe").exists():
        run([sys.executable, "-m", "venv", tools])
        run([tools / "Scripts" / "python.exe", "-m", "pip", "install", "-q", "meson", "ninja", "pkgconf", "pyyaml"])

    bin_dir = toolchain()
    posix = lambda p: str(p).replace("\\", "/")
    native = OUT / "native.ini"
    native.write_text(f"""[binaries]
c = '{posix(bin_dir / "clang.exe")}'
cpp = '{posix(bin_dir / "clang++.exe")}'
ar = '{posix(bin_dir / "llvm-ar.exe")}'
strip = '{posix(bin_dir / "llvm-strip.exe")}'
windres = '{posix(bin_dir / "llvm-windres.exe")}'
pkg-config = '{posix(tools / "Scripts" / "pkgconf.exe")}'
python3 = '{posix(tools / "Scripts" / "python.exe")}'
python = '{posix(tools / "Scripts" / "python.exe")}'

[built-in options]
pkg_config_path = '{posix(sysroot / "lib" / "pkgconfig")}'
c_args = ['-I{posix(sysroot / "include")}', '-Wno-strict-prototypes', '-Wno-error=strict-prototypes']
c_link_args = ['-L{posix(sysroot / "lib")}']
""")
    build = src / "builddir"
    shutil.rmtree(build, ignore_errors=True)
    run([tools / "Scripts" / "meson.exe", "setup", build, "--native-file", native,
         "-Dvenus=true", "-Dvideo=false", "-Dtests=false", "-Dbuildtype=release"], cwd=src)
    run([tools / "Scripts" / "ninja.exe", "-C", build])

    (OUT / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / "src" / "libvirglrenderer-1.dll", LIBRARY)
    for dll in ("libepoxy-0.dll", "libwinpthread-1.dll"):
        shutil.copy2(sysroot / "bin" / dll, OUT / "bin" / dll)
    print(f"Venus is in {(OUT / 'bin').relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
