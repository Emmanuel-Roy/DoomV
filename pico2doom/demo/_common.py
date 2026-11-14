"""Shared by the demo scripts: paths, the recording format, running DoomV.

A recording (.p2dl) is the byte stream the Pico sends on its link port, as
port/port_link.h defines it. picolink.py writes it to a file as it arrives.
"""
import os
import re
import struct
import subprocess
import sys
import time

DEMO = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(DEMO)                       # pico2doom/
sys.path.insert(0, os.path.join(REPO, "tools"))
import picolink                                    # noqa: E402  (find_doomv, GUEST_MARCH)

OUT = os.path.join(REPO, "build", "demo")          # everything the demos write
GUEST = os.path.join(REPO, "build", "firmware", "doomv-guest")
PICOLINK = os.path.join(REPO, "tools", "picolink.py")

TICKS, TICKS_REP, KEY, NOKEY, FRAME, HELLO, VIDEO, PALETTE, TEXT, ARGS, INPUT = range(1, 12)
FIXED = {TICKS: 4, TICKS_REP: 1, KEY: 2, NOKEY: 0, FRAME: 20, HELLO: 16, VIDEO: 4 + 320 * 200,
         PALETTE: 768, INPUT: 3}

PROGRESS = re.compile(r"picolink: \d+ frames in step")   # DoomV's every-175-frames line

# DOOM's key codes (doomkeys.h) for the keys a player presses most.
KEY_NAMES = {0xAD: "up", 0xAF: "down", 0xAC: "left", 0xAE: "right", 0xA3: "fire", 0xA2: "use",
             0xB8: "alt (strafe)", 0x1B: "escape", 0x0D: "enter", 0x09: "tab (map)", 0xB6: "shift (run)"}


def key_name(k):
    return KEY_NAMES.get(k, chr(k) if 32 < k < 127 else f"key {k:#x}")


def records(data):
    """Yield (offset, tag, payload offset) for every record."""
    i = 0
    while i < len(data):
        tag = data[i]
        if tag in (TEXT, ARGS):
            if i + 2 > len(data):
                return
            n = 1 + data[i + 1]
        elif tag in FIXED:
            n = FIXED[tag]
        else:
            sys.exit(f"demo: unknown record tag {tag:#x} at byte {i} -- not a Pico2Doom recording?")
        if i + 1 + n > len(data):
            return                                 # a recording cut off mid-record
        yield i, tag, i + 1
        i += 1 + n


def frames(data):
    """[(frame number, picture hash, state hash, instret logic, instret render, record offset)]."""
    out = []
    for off, tag, p in records(data):
        if tag == FRAME:
            out.append(struct.unpack_from("<5I", data, p) + (off,))
    return out


def rel(path):
    try:
        return os.path.relpath(path)
    except ValueError:                             # another drive on Windows
        return path


def need_guest():
    elf = os.path.join(GUEST, "pico2doom-doomv.elf")
    wad = os.path.join(GUEST, "doom1.wad")
    if not (os.path.isfile(elf) and os.path.isfile(wad)):
        sys.exit("demo: no DoomV guest -- build the firmware first (it builds the guest too):\n"
                 "  cmake --build build/firmware")
    return elf, wad


def record(path, seconds, warp=None, video=2):
    """Record the Pico into path with picolink.py (reboots it into link mode)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cmd = [sys.executable, "-u", PICOLINK, "record", path, "--seconds", str(seconds), "--video", str(video)]
    if warp:
        cmd += ["--warp", str(warp[0]), str(warp[1])]
    banner(f"Recording the Pico for {seconds} s -> {rel(path)}"
           + ("\n  (click the Pico window to play with the keyboard)" if video else ""))
    r = subprocess.run(cmd)
    if r.returncode or not os.path.isfile(path):
        sys.exit("demo: recording failed")
    n = len(frames(open(path, "rb").read()))
    print(f"\ndemo: recorded {n} frames")
    return n


def replay(path, workdir, headless=True, extra=(), quiet=False, timeout=None, max_frames=None):
    """Replay a recording in DoomV until its last frame is checked or DoomV
    halts at a difference. Returns (frames matched, total frames, DoomV's
    picolink lines, diverged?)."""
    elf, wad = need_guest()
    doomv = picolink.find_doomv(None)
    total = len(frames(open(path, "rb").read()))
    if max_frames:
        total = min(total, max_frames)
    os.makedirs(workdir, exist_ok=True)
    cmd = [doomv, wad, elf, picolink.GUEST_MARCH, "-picolink=" + os.path.abspath(path)] + list(extra)
    if headless:
        cmd.append("-headless")
    env = dict(os.environ, PICOLINK_ACK="1")
    p = subprocess.Popen(cmd, cwd=workdir, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True, errors="replace")
    matched, lines, diverged = 0, [], False
    start = time.time()
    try:
        for line in p.stderr:
            acks = re.findall(r"picolink-ack (\d+)", line)
            if acks:
                matched = int(acks[-1])
                if not quiet and matched % 35 == 0:
                    print(f"\r  DoomV: {matched}/{total} frames identical", end="", flush=True)
                if matched >= total:
                    break
                continue
            if line.startswith("picolink:") or line.startswith("  "):
                if "Pico says" in line:
                    continue
                # Live: only the opening line; the caller reports the outcome.
                if "replaying" in line:
                    if not quiet:
                        print("  " + line.rstrip())
                    continue
                lines.append(line.rstrip())
                if "DIVERGED" in line:
                    diverged = True
                if "machine halted" in line:
                    break
            if timeout and time.time() - start > timeout:
                break
    finally:
        # A divergence's report lines all arrive before "machine halted".
        p.terminate()
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
    if not quiet:
        print()
    return matched, total, lines, diverged


def ppm_to_png(ppm, png):
    """picolink_doomv.ppm (P6) -> PNG, with only the standard library."""
    import zlib
    data = open(ppm, "rb").read()
    parts = data.split(maxsplit=3)                 # P6, width, height, then "255\n" + pixels
    w, h = int(parts[1]), int(parts[2])
    px = data[len(data) - w * h * 3:]
    raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(png, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def banner(text):
    print("\n" + "=" * 70)
    for line in text.splitlines():
        print("  " + line)
    print("=" * 70)
