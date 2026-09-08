#!/usr/bin/env python3
"""Compile DTBs with initrd addresses matching the actual packaged archives."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
INITRD_START = 0x82300000
RAM_END = 0x90000000


def device_tree(source, initrd_size, smoke=False):
    if not 0 < initrd_size <= RAM_END - INITRD_START:
        raise ValueError("initramfs is empty or extends past guest RAM")
    for name, value in (("start", INITRD_START), ("end", INITRD_START + initrd_size)):
        source, count = re.subn(rf"linux,initrd-{name}\s*=\s*<[^>]+>;",
                                f"linux,initrd-{name} = <0x{value:x}>;", source)
        if count != 1:
            raise ValueError(f"expected exactly one linux,initrd-{name} property")
    if smoke:
        if source.count("rdinit=/bin/sh") != 2:
            # There is normally one explanatory comment and one bootargs value.
            if not re.search(r'bootargs\s*=\s*"[^"\n]*rdinit=/bin/sh', source):
                raise ValueError("bootargs is missing rdinit=/bin/sh")
        source = source.replace("rdinit=/bin/sh", "rdinit=/doomv-smoke")
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path)
    args = parser.parse_args()
    source = (ROOT / "tools/linux/dts/doomv.dts").read_text()
    for stem, archive, smoke in (("doomv", "initramfs.cpio", False), ("smoke", "smoke.cpio", True)):
        dts = args.images / f"{stem}.dts"
        dts.write_text(device_tree(source, (args.images / archive).stat().st_size, smoke))
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dts.with_suffix(".dtb")), str(dts)], check=True)


if __name__ == "__main__":
    main()
