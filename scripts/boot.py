#!/usr/bin/env python3
"""Boot a DoomV guest.

  python scripts/boot.py doom                      # DOOM
  python scripts/boot.py linux                     # Linux with a BusyBox shell
  python scripts/boot.py ubuntu                    # Ubuntu 24.04 from ubuntu.img

Every guest takes --ram, --headless and --no-build; the Linux ones also take
--harts. `boot.py <guest> --help` lists the rest, including advanced options
(snapshots, stop-at, record/replay) and `--` for raw emulator options.
"""
import sys

from boot_doom import main as doom_main
from boot_linux import main as linux_main
from boot_ubuntu import main as ubuntu_main
from common import entrypoint

GUESTS = {"doom": doom_main, "linux": linux_main, "ubuntu": ubuntu_main}


def main():
    # The guest is taken off first, so `boot.py ubuntu --help` reaches the
    # ubuntu script's help rather than this one's.
    if len(sys.argv) < 2 or sys.argv[1] not in GUESTS:
        print(__doc__.strip())
        return 0 if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help") else 2
    guest = sys.argv[1]
    sys.argv = [sys.argv[0]] + sys.argv[2:]
    return entrypoint(GUESTS[guest])


if __name__ == "__main__":
    sys.exit(main())
