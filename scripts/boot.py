#!/usr/bin/env python3
"""Boot DoomV guests.

Examples:
  python scripts/boot.py doom
  python scripts/boot.py linux --smoke
  python scripts/boot.py linux --no-build
  python scripts/boot.py ubuntu
  python scripts/boot.py ubuntu --login
"""
import argparse
import sys

from boot_doom import main as doom_main
from boot_linux import main as linux_main
from boot_ubuntu import main as ubuntu_main
from common import entrypoint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("guest", choices=("doom", "linux", "ubuntu"))
    args, rest = parser.parse_known_args()
    # Delegate parsing of guest-specific options to the focused implementation.
    sys.argv = [sys.argv[0]] + rest
    # Through entrypoint, as each boot_*.py is when run on its own: a missing
    # tool or input is one ERROR line naming it, not a traceback that buries
    # the build script's message.
    return entrypoint({"doom": doom_main, "linux": linux_main, "ubuntu": ubuntu_main}[args.guest])


if __name__ == "__main__":
    sys.exit(main())
