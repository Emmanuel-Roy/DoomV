#!/usr/bin/env python3
"""Boot DoomV guests.

Examples:
  python scripts/boot.py doom
  python scripts/boot.py linux --smoke
  python scripts/boot.py linux --no-build
"""
import argparse
import sys

from boot_doom import main as doom_main
from boot_linux import main as linux_main


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("guest", choices=("doom", "linux"))
    args, rest = parser.parse_known_args()
    # Delegate parsing of guest-specific options to the focused implementation.
    sys.argv = [sys.argv[0]] + rest
    return doom_main() if args.guest == "doom" else linux_main()


if __name__ == "__main__":
    sys.exit(main())
