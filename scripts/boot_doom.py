#!/usr/bin/env python3
"""Build and play bare-metal DOOM.

  python scripts/boot.py doom                   # the shareware episode
  python scripts/boot.py doom --game doom2      # needs your own DOOM2.WAD
"""
import argparse
from pathlib import Path
import sys

from common import (ROOT, add_advanced_options, add_boot_options, boot_args, build_emulator, checkout_lock, entrypoint,
                    require_files, run, split_passthrough, wsl_path, wsl_script)

WADS = {"free": "DOOM1.WAD", "doom": "DOOM.WAD", "doom2": "DOOM2.WAD", "finaldoom": "TNT.WAD"}


def main():
    argv, passthrough = split_passthrough(sys.argv[1:])
    parser = argparse.ArgumentParser(prog="boot.py doom", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    g = parser.add_argument_group("doom")
    g.add_argument("--game", choices=tuple(WADS), default="free",
                   help="which game (default free, the shareware DOOM1.WAD)")
    g.add_argument("--wad", type=Path, help="a WAD somewhere else (must match --game)")
    add_boot_options(parser, linux=False)
    add_advanced_options(parser, linux=False)
    args = parser.parse_args(argv)

    guest = ROOT / "tools/doom/doombuild"
    wad = args.wad.resolve() if args.wad else guest / WADS[args.game]
    elf = guest / f"doomv-{args.game}.elf"
    require_files(wad)
    with checkout_lock():
        if not args.no_build:
            build_emulator()
            wsl_script("build_doom.sh", args.game, wsl_path(wad))
        require_files(ROOT / "riscv_doom.exe", elf)
        if not args.headless:
            print("DOOM opens in the emulator window. Ctrl+Alt+G grabs the mouse; close the window to exit.")
        run([ROOT / "riscv_doom.exe", *boot_args(args, passthrough), wad, elf])
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
