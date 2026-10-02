#!/usr/bin/env python3
"""A device tree for a machine of several harts, from one written for one.

DoomV's -harts=N gives each hart its own CLINT msip and mtimecmp and its own
pair of IMSIC files, hart h's at the M and S bases + h * 0x1000 (see "Several
harts" in the README). The device trees here describe one hart. This writes
the same tree for N:

  * cpu0 repeated as cpu1..cpuN-1, each with its own interrupt controller
  * the CLINT's interrupts-extended naming every hart's M software and timer
  * each IMSIC node's region covering N files, and its interrupts-extended
    naming every hart's external interrupt (M or S), in hart order, which is
    the order the files are in

and compiles it with dtc (in WSL, as scripts/build_linux.sh does):

  python tools/linux/dts/smp.py build/linux/doomv.dts 4      # -> build/linux/doomv-h4.dtb
  riscv_doom.exe -harts=4 -dtb=build/linux/doomv-h4.dtb ...
"""
import pathlib
import re
import subprocess
import sys


def block_end(text: str, start: int) -> int:
    """The index just past the `};` closing the node whose `{` follows start."""
    depth = 0
    i = text.index("{", start)
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text.index(";", i) + 1
        elif text.startswith("//", i):
            i = text.index("\n", i)
            continue
        i += 1
    raise ValueError("unbalanced braces")


def smp(text: str, n: int) -> str:
    m = re.search(r"^(\s*)cpu0: cpu@0 \{", text, re.M)
    if not m:
        raise ValueError("no `cpu0: cpu@0` node")
    start, end = m.start(), block_end(text, m.start())
    cpu0 = text[start:end]
    # The copies without cpu0's commentary, which is about the hart, not this.
    plain = "\n".join(l for l in cpu0.split("\n") if not l.strip().startswith("//"))
    copies = []
    for h in range(1, n):
        c = plain.replace("cpu0: cpu@0", f"cpu{h}: cpu@{h}", 1)
        c = re.sub(r"reg = <0>;", f"reg = <{h}>;", c, count=1)
        c = c.replace("cpu0_intc:", f"cpu{h}_intc:")
        copies.append(c)
    text = text[:end] + "".join("\n" + c for c in copies) + text[end:]

    def every(*irqs):
        return " ".join(f"&cpu{h}_intc {i}" for h in range(n) for i in irqs)

    clint = "interrupts-extended = <&cpu0_intc 3 &cpu0_intc 7>;"
    if clint not in text:
        raise ValueError("no single-hart CLINT interrupts-extended")
    text = text.replace(clint, f"interrupts-extended = <{every(3, 7)}>;")
    for base, irq in (("0x24000000", 11), ("0x28000000", 9)):
        reg = f"reg = <0x0 {base} 0x0 0x1000>;"
        ext = f"interrupts-extended = <&cpu0_intc {irq}>;"
        if reg not in text or ext not in text:
            raise ValueError(f"no single-hart IMSIC node at {base}")
        text = text.replace(reg, f"reg = <0x0 {base} 0x0 {0x1000 * n:#x}>;")
        text = text.replace(ext, f"interrupts-extended = <{every(irq)}>;")
    return text


def wsl(p: pathlib.Path) -> str:
    s = str(p.resolve()).replace("\\", "/")
    return "/mnt/" + s[0].lower() + s[2:]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = pathlib.Path(sys.argv[1])
    n = int(sys.argv[2])
    dts = src.with_name(f"{src.stem}-h{n}.dts")
    dtb = dts.with_suffix(".dtb")
    dts.write_text(smp(src.read_text(encoding="utf-8"), n), encoding="utf-8", newline="\n")
    subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", "dtc", "-q", "-I", "dts", "-O", "dtb",
                    "-o", wsl(dtb), wsl(dts)], check=True)
    print(dtb)


if __name__ == "__main__":
    main()
