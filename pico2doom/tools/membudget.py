#!/usr/bin/env python3
"""Where the Pico 2's memory goes, read from the firmware ELF.

Prints flash use (code, data, the packed WAD) and SRAM use (static data,
what is left for DOOM's zone), plus the largest RAM symbols. Run after every
build by firmware/CMakeLists.txt; no tools needed besides Python.

Usage: python tools/membudget.py build/firmware/pico2doom.elf
"""
import struct
import sys

FLASH = 4 * 1024 * 1024
SRAM_MAIN = 512 * 1024

def read_elf(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 1, "expects a 32-bit ELF"
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = \
            struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link))
    strtab = secs[shstrndx]
    for s in secs:
        n = d[strtab["off"] + s["name"]:]
        s["name"] = n[:n.index(0)].decode()
    syms = {}
    for s in secs:
        if s["type"] != 2:  # SHT_SYMTAB
            continue
        names = secs[s["link"]]
        for i in range(s["size"] // 16):
            st_name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, s["off"] + 16 * i)
            n = d[names["off"] + st_name:]
            syms[n[:n.index(0)].decode()] = (value, size)
    return secs, syms

def main():
    secs, syms = read_elf(sys.argv[1])
    alloc = [s for s in secs if s["flags"] & 2 and s["size"]]
    flash = sum(s["size"] for s in alloc if 0x10000000 <= s["addr"] < 0x14000000)
    # .data lives in RAM but its initial values are also stored in flash.
    data = sum(s["size"] for s in alloc if s["addr"] >= 0x20000000 and s["type"] == 1)
    ram_static = sum(s["size"] for s in alloc if 0x20000000 <= s["addr"] < 0x20080000)
    wad = syms.get("doom_wad_end", (0, 0))[0] - syms.get("doom_wad", (0, 0))[0]

    print("memory budget  (Pico 2: 4096 KB flash, 512 KB SRAM + 8 KB stack)")
    print(f"  flash  {(flash + data) / 1024:7.1f} KB used  = code+data {(flash + data - wad) / 1024:.1f} KB"
          f" + WAD {wad / 1024:.1f} KB   ({FLASH / 1024 - (flash + data) / 1024:.1f} KB free)")
    print(f"  SRAM   {ram_static / 1024:7.1f} KB static (.data + .bss)  ->  "
          f"{(SRAM_MAIN - ram_static) / 1024:.1f} KB heap for DOOM's zone + malloc")
    big = sorted(((v[1], n) for n, v in syms.items() if 0x20000000 <= v[0] < 0x20082000 and v[1] >= 2048),
                 reverse=True)[:12]
    print("  largest RAM objects: " + ", ".join(f"{n} {sz // 1024} KB" for sz, n in big))

if __name__ == "__main__":
    main()
