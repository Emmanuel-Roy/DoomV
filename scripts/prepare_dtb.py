#!/usr/bin/env python3
"""Compile the guest device trees.

Two of the three carry an initramfs, so their linux,initrd-* addresses have
to match the archives actually packaged next to them. The third boots a real
disk and must carry no initrd at all.
"""
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


def disk_device_tree(source):
    """The Ubuntu variant: root on the disk, and no initramfs at all.

    The two edits are not independent. Leaving linux,initrd-start pointing at
    an address nothing was loaded to is how you get a kernel that unpacks
    garbage and panics; and with a *valid* initramfs present the kernel runs
    that and never mounts the disk, which looks like a successful boot of
    nothing. So the properties are removed rather than adjusted.
    """
    source, count = re.subn(r'bootargs = "[^"]*";',
                            'bootargs = "earlycon=sbi console=tty0 console=hvc0 '
                            'root=/dev/vda1 rootwait rw";', source)
    if count != 1:
        raise ValueError("expected exactly one bootargs property")
    source, count = re.subn(r"\s*linux,initrd-(start|end)\s*=\s*<[^>]*>;", "", source)
    if count != 2:
        raise ValueError("expected exactly two linux,initrd-* properties to remove")
    return source


def compile_dtb(dts: Path, source: str):
    dts.write_text(source)
    subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dts.with_suffix(".dtb")), str(dts)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path)
    args = parser.parse_args()
    source = (ROOT / "tools/linux/dts/doomv.dts").read_text()
    for stem, archive, smoke in (("doomv", "initramfs.cpio", False), ("smoke", "smoke.cpio", True)):
        compile_dtb(args.images / f"{stem}.dts",
                    device_tree(source, (args.images / archive).stat().st_size, smoke))
    # Built here rather than by the Ubuntu boot script, so that every device
    # tree this project uses is produced in one place from one source, and so
    # that booting the image needs no WSL round trip for dtc.
    compile_dtb(args.images / "ubuntu.dts", disk_device_tree(source))


if __name__ == "__main__":
    main()
