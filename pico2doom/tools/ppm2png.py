#!/usr/bin/env python3
"""Convert the simulator's PPM frame dumps to PNG (no dependencies).

Usage: python tools/ppm2png.py frame00060.ppm [more.ppm ...]
"""
import struct
import sys
import zlib

def ppm_to_png(src, dst):
    data = open(src, "rb").read()
    parts = data.split(maxsplit=4)
    assert parts[0] == b"P6", "not a binary PPM"
    w, h = int(parts[1]), int(parts[2])
    pixels = parts[4][: w * h * 3]
    raw = b"".join(b"\0" + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(dst, "wb").write(png)

if __name__ == "__main__":
    for f in sys.argv[1:]:
        out = f.rsplit(".", 1)[0] + ".png"
        ppm_to_png(f, out)
        print(out)
