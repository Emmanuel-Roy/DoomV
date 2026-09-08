#!/usr/bin/env python3
"""Build DoomV, its DOOM guest, Linux images, or everything.

Examples:
  python scripts/build.py doom
  python scripts/build.py linux
  python scripts/build.py all
"""
import argparse
import sys

from common import build_emulator, checkout_lock, entrypoint, wsl_path, wsl_script, ROOT, require_files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("doom", "linux", "all"), nargs="?", default="all")
    parser.add_argument("--game", choices=("free", "doom", "doom2", "finaldoom"), default="free")
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
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
