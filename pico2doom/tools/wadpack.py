#!/usr/bin/env python3
"""Pack DOOM1.WAD so it fits in the Pico 2's 4 MB flash next to the firmware.

The shareware IWAD is 4,196,020 bytes -- 1.7 KB *more* than the whole flash
chip -- so something has to go. Real compression would not help: DOOM reads
lumps straight out of the memory-mapped WAD (w_wad.c returns a pointer into
the mapping instead of copying), and a compressed lump would have to be
inflated into RAM, which is the resource we have least of (520 KB).

So the packer removes what this hardware cannot use and keeps everything
else byte-for-byte, still in the standard WAD format:

  * sound effects (DS*), PC-speaker sounds (DP*), music (D_*), and the
    music synth banks (GENMIDI, DMXGUS) become zero-length lumps. Their
    directory entries stay, so W_GetNumForName() still finds them and the
    (silent) sound code never hits "lump not found".
  * byte-identical lumps are stored once and share an offset.
  * every lump starts on a 4-byte boundary. RISC-V faults on misaligned
    word loads from flash in some paths, and DOOM casts lump pointers to
    int/short structs freely.

The output is an ordinary IWAD: DoomV, Chocolate Doom, etc. can load it.

Usage:  python tools/wadpack.py DOOM1.WAD build/doom1-packed.wad
"""
import argparse
import hashlib
import struct
import sys

SHAREWARE_SHA1 = "5b2e249b9c5133ec987b3ea77596381dc0d6bc1d"  # DOOM1.WAD v1.9

def is_audio(name: str) -> bool:
    return (name.startswith(("DS", "DP", "D_")) and not name.startswith("DEMO")) \
        or name in ("GENMIDI", "DMXGUS")

def pack(data: bytes, keep_audio: bool = False):
    ident, count, dir_off = struct.unpack_from("<4sii", data, 0)
    if ident not in (b"IWAD", b"PWAD"):
        raise SystemExit("not a WAD file")

    entries = []
    for i in range(count):
        pos, size, raw = struct.unpack_from("<ii8s", data, dir_off + 16 * i)
        entries.append((raw, raw.rstrip(b"\0").decode("ascii", "replace"), pos, size))

    body = bytearray(b"\0" * 12)       # header patched at the end
    seen = {}                          # sha1 -> offset of first copy
    directory = []
    stats = {"dropped": 0, "dropped_bytes": 0, "dedup": 0, "dedup_bytes": 0}

    for raw, name, pos, size in entries:
        if size and is_audio(name) and not keep_audio:
            stats["dropped"] += 1
            stats["dropped_bytes"] += size
            directory.append((0, 0, raw))
            continue
        if size == 0:                  # markers (F_START, E1M1, ...)
            directory.append((0, 0, raw))
            continue
        blob = data[pos:pos + size]
        key = hashlib.sha1(blob).digest()
        if key in seen:
            stats["dedup"] += 1
            stats["dedup_bytes"] += size
            directory.append((seen[key], size, raw))
            continue
        while len(body) % 4:
            body.append(0)
        seen[key] = len(body)
        directory.append((len(body), size, raw))
        body += blob

    while len(body) % 4:
        body.append(0)
    new_dir = len(body)
    for pos, size, raw in directory:
        body += struct.pack("<ii8s", pos, size, raw)
    struct.pack_into("<4sii", body, 0, ident, count, new_dir)
    return bytes(body), stats

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--keep-audio", action="store_true",
                    help="keep sound and music lumps (for a build with audio)")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    sha1 = hashlib.sha1(data).hexdigest()
    if sha1 != SHAREWARE_SHA1:
        print(f"wadpack: note: {args.input} is not shareware DOOM1.WAD v1.9 "
              f"(sha1 {sha1}); packing anyway", file=sys.stderr)

    out, st = pack(data, args.keep_audio)
    open(args.output, "wb").write(out)
    print(f"wadpack: {len(data):,} -> {len(out):,} bytes "
          f"({100 * len(out) / len(data):.1f}%); dropped {st['dropped']} audio lumps "
          f"({st['dropped_bytes']:,} B), merged {st['dedup']} duplicates "
          f"({st['dedup_bytes']:,} B)")

if __name__ == "__main__":
    main()
