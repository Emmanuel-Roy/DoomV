#!/usr/bin/env python3
"""Build and play bare-metal DOOM in the native DoomV window."""
import argparse
from pathlib import Path
import sys

from common import ROOT, build_emulator, checkout_lock, entrypoint, require_files, run, wsl_script, wsl_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", choices=("free", "doom", "doom2", "finaldoom"), default="free")
    parser.add_argument("--wad", type=Path, help="override the WAD path (must match --game)")
    parser.add_argument("--no-build", action="store_true", help="use existing emulator and guest ELF")
    args = parser.parse_args()
    guest = ROOT / "tools/doom/doombuild"
    names = {"free": "DOOM1.WAD", "doom": "DOOM.WAD", "doom2": "DOOM2.WAD", "finaldoom": "TNT.WAD"}
    wad = args.wad.resolve() if args.wad else guest / names[args.game]
    elf = guest / f"doomv-{args.game}.elf"
    require_files(wad)
    with checkout_lock():
        if not args.no_build:
            build_emulator()
            wsl_script("build_doom.sh", args.game, wsl_path(wad))
        require_files(ROOT / "riscv_doom.exe", elf)
        print("DOOM opens in the emulator window. Close the window to exit.")
        run([ROOT / "riscv_doom.exe", wad, elf])
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
