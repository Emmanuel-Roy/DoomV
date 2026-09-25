#!/usr/bin/env python3
"""Boot DoomV guests.

Examples:
  python scripts/boot.py doom
  python scripts/boot.py linux --smoke
  python scripts/boot.py linux --no-build
  python scripts/boot.py ubuntu
  python scripts/boot.py ubuntu --login

  python scripts/boot.py linux --ram 4G          # more memory for any guest
  python scripts/boot.py ubuntu --ram 8G

`<guest> --help` lists what that guest takes, which is not the same set for
each of the three.
"""
import argparse
import sys

from boot_doom import main as doom_main
from boot_linux import main as linux_main
from boot_ubuntu import main as ubuntu_main
from common import entrypoint


GUESTS = ("doom", "linux", "ubuntu")


def main():
    # The guest is matched before argparse sees anything, so that `--help`
    # after a guest name reaches that guest rather than being answered here.
    # argparse acts on -h the moment it reads it, so with a parser in front
    # `boot.py ubuntu --help` printed this file's options -- the two lines
    # about which guests exist -- and never the ubuntu ones, which is the
    # opposite of what was asked for.
    if len(sys.argv) > 1 and sys.argv[1] in GUESTS:
        guest, rest = sys.argv[1], sys.argv[2:]
    else:
        parser = argparse.ArgumentParser(description=__doc__,
                                         formatter_class=argparse.RawDescriptionHelpFormatter)
        parser.add_argument("guest", choices=GUESTS)
        args, rest = parser.parse_known_args()
        guest = args.guest
    # Delegate parsing of guest-specific options to the focused implementation.
    sys.argv = [sys.argv[0] + " " + guest] + rest
    # Through entrypoint, as each boot_*.py is when run on its own: a missing
    # tool or input is one ERROR line naming it, not a traceback that buries
    # the build script's message.
    return entrypoint({"doom": doom_main, "linux": linux_main, "ubuntu": ubuntu_main}[guest])


if __name__ == "__main__":
    sys.exit(main())
