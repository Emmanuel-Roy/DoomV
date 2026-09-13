#!/usr/bin/env python3
"""Create a storage drive for the Linux guest: drives/<name>.img.

A raw disk image, formatted ext4 and labelled with its name, so it mounts
straight away inside the guest -- `mount /dev/vdb /mnt` or `LABEL=<name>`.
Every *.img in drives/ is attached at boot; see drives/README.md.

  python scripts/mkdrive.py data 1G
  python scripts/mkdrive.py scratch 256M
  python scripts/mkdrive.py blank 64M --no-format   # unformatted
"""
import argparse
import re
import sys

from common import ROOT, entrypoint, run, wsl_path
import os

DRIVES = ROOT / "drives"
UNITS = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30, "T": 1 << 40}


def parse_size(text):
    match = re.fullmatch(r"\s*(\d+)\s*([KMGT]?)i?B?\s*", text, re.IGNORECASE)
    if not match:
        raise argparse.ArgumentTypeError(f"not a size: {text!r} (try 512M or 2G)")
    size = int(match.group(1)) * UNITS.get(match.group(2).upper(), 1)
    # ext4 wants a few megabytes to lay out its metadata, and a disk is
    # addressed in whole 512-byte sectors.
    if size < 8 * UNITS["M"]:
        raise argparse.ArgumentTypeError("a drive has to be at least 8M")
    return size - size % 512


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("name", help="drive name; the file is drives/<name>.img")
    parser.add_argument("size", type=parse_size, help="size, e.g. 512M, 1G, 20G")
    parser.add_argument("--no-format", action="store_true",
                        help="leave it blank instead of formatting it ext4")
    args = parser.parse_args()

    # The name becomes a filename and an ext4 label, which is at most 16
    # bytes. Keep it to characters that are safe as both.
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,16}", args.name):
        parser.error("name must be 1-16 letters, digits, '-' or '_'")

    DRIVES.mkdir(exist_ok=True)
    image = DRIVES / f"{args.name}.img"
    if image.exists():
        raise RuntimeError(f"{image} already exists; delete it first if you mean to replace it")

    # Sparse where the filesystem supports it: the file reports its full
    # size, but blocks nobody has written take no space on the host.
    with open(image, "wb") as f:
        f.truncate(args.size)

    if not args.no_format:
        try:
            # mke2fs from WSL, the same place the rest of the Linux tooling
            # here comes from. -F because it is a file, not a block device.
            run(["wsl.exe", "-d", os.environ.get("DISTRO", "Ubuntu"), "-u", "root", "--",
                 "mkfs.ext4", "-q", "-F", "-L", args.name, wsl_path(image)])
        except BaseException:
            image.unlink(missing_ok=True)
            raise

    kind = "blank" if args.no_format else "ext4"
    print(f"Created {image} ({args.size // UNITS['M']} MiB, {kind}).")
    print("It is attached the next time Linux boots. Inside the guest:")
    print(f"  mkdir -p /mnt/{args.name} && mount LABEL={args.name} /mnt/{args.name}"
          if not args.no_format else "  cat /proc/partitions")
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
