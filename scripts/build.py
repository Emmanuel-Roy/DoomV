#!/usr/bin/env python3
"""Build DoomV, its DOOM guest, Linux images, or everything.

`all` finishes by rebuilding the emulator with profile-guided optimization and
LTO, which is worth ~1.36x on an Ubuntu boot and ~1.4x on DOOM over the plain
build. That step needs the guests, because training means running them -- which
is why it happens here, at the end of `all`, and not in the Makefile: `make`
has to work on a checkout with nothing built yet, and is what the test gate
uses. --no-pgo skips it.

Examples:
  python scripts/build.py doom
  python scripts/build.py linux
  python scripts/build.py all
  python scripts/build.py all --no-pgo      # stop at the plain build
"""
import argparse
import subprocess
import sys

from common import build_emulator, checkout_lock, entrypoint, wsl_path, wsl_script, ROOT, require_files

# What training needs: both of bench.py's core workloads actually run.
TRAINING_INPUTS = (
    ROOT / "build/linux/fw_jump.elf", ROOT / "build/linux/Image",
    ROOT / "build/linux/doomv.dtb", ROOT / "build/linux/initramfs.cpio",
    ROOT / "tools/doom/doombuild/DOOM1.WAD", ROOT / "tools/doom/doombuild/doomv-free.elf",
)


def build_pgo():
    """Rebuild riscv_doom.exe with PGO and LTO, if the training inputs exist.

    Missing inputs are not an error: `build.py doom` builds one guest and
    cannot train on the other, and saying so is more useful than either
    failing or silently producing a slower binary than asked for.
    """
    missing = [p for p in TRAINING_INPUTS if not p.exists()]
    if missing:
        print("Skipping the PGO build: training needs both workloads, and these are missing:")
        for p in missing:
            # Repo-relative when it can be, absolute otherwise: this is an
            # error path, and it should not be the thing that raises.
            try:
                print(f"  {p.relative_to(ROOT)}")
            except ValueError:
                print(f"  {p}")
        print("Run `python scripts/build.py all` for the optimized emulator.")
        return
    print("Rebuilding with PGO and LTO (this runs both workloads to train on)...", flush=True)
    subprocess.run([sys.executable, str(ROOT / "performance/pgo.py")], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("doom", "linux", "all"), nargs="?", default="all")
    parser.add_argument("--game", choices=("free", "doom", "doom2", "finaldoom"), default="free")
    parser.add_argument("--no-pgo", action="store_true",
                        help="leave the plain build in place instead of rebuilding with PGO and LTO")
    args = parser.parse_args()
    with checkout_lock():
        if args.target in ("doom", "linux", "all"):
            build_emulator()
        if args.target in ("doom", "all"):
            wad_names = {"free": "DOOM1.WAD", "doom": "DOOM.WAD", "doom2": "DOOM2.WAD", "finaldoom": "TNT.WAD"}
            wad = ROOT / "tools/doom/doombuild" / wad_names[args.game]
            require_files(wad)
            wsl_script("build_doom.sh", args.game, wsl_path(wad))
        if args.target in ("linux", "all"):
            wsl_script("build_linux.sh")
        # Last, because it trains on the guests the steps above produce.
        if not args.no_pgo:
            build_pgo()
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
