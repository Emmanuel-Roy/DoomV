"""Demo 3: a divergence, caught on the frame it happens.

Takes a recording of the real Pico, changes one thing in a copy of it, and
replays the copy in DoomV. DoomV runs in step until the changed point and
stops at the first frame that differs, saying what differs. See
docs/DIVERGENCE.md for why each kind stops where it does.

Kinds of fault (--kind):
  hash     one bit of one frame's picture hash: the Pico got one result wrong
  instret  one frame's retired-instruction count +1: one extra instruction ran
  key      one key press swapped for another: an input delivered wrong
  clock    one clock reading 100 ms late: a timer read wrong

  python demo/divergence.py                       # --kind hash at the middle frame
  python demo/divergence.py --kind key
  python demo/divergence.py --kind clock --frame 400
  python demo/divergence.py --all                 # every kind, one after another
"""
import argparse
import os
import re
import struct
import sys

from _common import (FRAME, PROGRESS, KEY, OUT, TICKS, banner, frames, key_name, latest_recording, ppm_to_png,
                     record, records, rel, replay)

EXPLAIN = {
    "hash": "The changed hash is compared directly, so the very frame it is on fails. Everything\n"
            "  DoomV computed is still right -- the game state and instruction counts match: only\n"
            "  the picture's fingerprint disagrees, exactly what one wrong result on the Pico looks like.",
    "instret": "The Pico's retired-instruction count for that frame is one higher. The picture and the\n"
               "  game are identical, yet the frame fails: lock-step at retirement notices a single\n"
               "  instruction more or less, even one with no visible effect.",
    "key": "DoomV is given a different key than the Pico read. DOOM reads keys at the start of each\n"
           "  game tic and turns them into that tic's move, so the player moves differently in the\n"
           "  very frame the key arrives: the game state, the picture and both instruction counts\n"
           "  differ at once. One input delivered wrong is caught on its own frame.",
    "clock": "DoomV's DOOM is told 100 ms more have passed than the Pico's saw: 3 game tics (1/35 s\n"
             "  each) are due at once. It goes to run them, and a tic starts by reading the keys --\n"
             "  while the Pico's DOOM, with no tic due yet, read the clock again. The very next input\n"
             "  each asks for differs, so the I/O sequence itself splits: caught mid-frame, before\n"
             "  any picture is drawn.",
}


def inject(data, kind, frame_no):
    """A copy of data with one fault at frame frame_no. Returns (bytes, what was done, frame it landed in)."""
    b = bytearray(data)
    fr = frames(data)
    idx = {f[0]: i for i, f in enumerate(fr)}
    if frame_no not in idx:
        sys.exit(f"demo: no frame {frame_no} (recording has {fr[0][0]}..{fr[-1][0]})")
    i = idx[frame_no]
    off = fr[i][5]
    if kind in ("hash", "instret"):
        p = off + 1
        if kind == "hash":
            h = struct.unpack_from("<I", b, p + 4)[0]
            struct.pack_into("<I", b, p + 4, h ^ 1)
            return bytes(b), f"frame {frame_no}: picture hash {h:#010x} -> {h ^ 1:#010x} (one bit)", frame_no
        n = struct.unpack_from("<I", b, p + 16)[0]
        struct.pack_into("<I", b, p + 16, n + 1)
        return bytes(b), f"frame {frame_no}: retired instructions in rendering {n:,} -> {n + 1:,}", frame_no
    # Inputs of frame k are the records between frame k-1's FRAME record and frame k's.
    start = fr[i - 1][5] if i else 0
    cur = frame_no
    for o, tag, p in records(data):
        if o < start:
            continue
        if tag == FRAME:
            cur = struct.unpack_from("<I", data, p)[0] + 1
            continue
        if kind == "clock" and tag == TICKS:
            t = struct.unpack_from("<I", b, p)[0]
            struct.pack_into("<I", b, p, t + 100)
            return bytes(b), f"frame {cur}: clock reading {t} ms -> {t + 100} ms", cur
        if kind == "key" and tag == KEY and data[p] == 1:
            k = data[p + 1]
            new = 0xAF if k == 0xAD else 0xAD           # up <-> down; anything else -> up
            b[p + 1] = new
            # Its release too, or DoomV would hold the new key forever.
            for o2, tag2, p2 in records(data):
                if o2 > o and tag2 == KEY and data[p2] == 0 and data[p2 + 1] == k:
                    b[p2 + 1] = new
                    break
            return bytes(b), f"frame {cur}: key press '{key_name(k)}' -> '{key_name(new)}' (and its release)", cur
    sys.exit(f"demo: no {'key press' if kind == 'key' else 'clock reading'} at or after frame {frame_no}"
             + (" -- record with keyboard input (play in the Pico window)" if kind == "key" else ""))


def run(src, kind, frame_no):
    data = open(src, "rb").read()
    fr = frames(data)
    if frame_no is None:
        frame_no = fr[len(fr) // 2][0]
    bad, what, landed = inject(data, kind, frame_no)
    dst = os.path.join(OUT, f"{os.path.splitext(os.path.basename(src))[0]}-fault-{kind}.p2dl")
    open(dst, "wb").write(bad)
    banner(f"Divergence demo: --kind {kind}\n  recording  {rel(src)} ({len(fr)} frames)\n"
           f"  fault      {what}\n  replaying  {rel(dst)} in DoomV")
    work = os.path.join(OUT, f"divergence-{kind}")
    for old in ("crash.log", "picolink_doomv.ppm"):
        if os.path.exists(os.path.join(work, old)):
            os.remove(os.path.join(work, old))
    _, _, _, diverged = replay(dst, work, hold=True, pace=True, report=lambda *r: show(r, fr, work, kind, landed))
    return diverged


def show(result, fr, work, kind, landed):
    """The result, printed while DoomV still shows the frame it stopped on."""
    matched, total, lines, diverged = result
    if not diverged:
        banner(f"RESULT: no divergence -- all {matched} frames matched. (A fault with no effect on DOOM?)")
        return
    report = [l for l in lines if not PROGRESS.match(l)]
    # The frame being run when DoomV stopped: the one after the last that matched.
    at = fr[matched][0] if matched < len(fr) else None
    seq = any("next input" in l or "asked for" in l for l in report)
    ppm = os.path.join(work, "picolink_doomv.ppm")
    png = os.path.join(work, "doomv_frame.png")
    if os.path.exists(ppm):
        ppm_to_png(ppm, png)
    banner("RESULT: DIVERGED\n" + "\n".join(report)
           + (f"\n\n  fault in frame {landed}, caught in frame {at}"
              + (" (the input sequence, mid-frame)" if seq else "") if at is not None else "")
           + (f" ({at - landed} frame{'s' if at - landed != 1 else ''} later)" if at is not None and at != landed else "")
           + f"\n  in step up to there: {matched} frames"
           + (f"\n  DoomV's picture at the stop: {rel(png)}; its registers: {rel(os.path.join(work, 'crash.log'))}"
              if os.path.exists(png) else "")
           + "\n\n  Why: " + EXPLAIN[kind])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("recording", nargs="?", help="a .p2dl (default: the newest in build/demo, or record one)")
    ap.add_argument("--kind", choices=sorted(EXPLAIN), default="hash")
    ap.add_argument("--all", action="store_true", help="run every kind")
    ap.add_argument("--frame", type=int, help="where to put the fault (default: the middle frame)")
    ap.add_argument("--record", type=float, metavar="SECONDS", help="record a new run first")
    a = ap.parse_args()
    src = a.recording or (None if a.record else latest_recording())
    if not src:
        src = os.path.join(OUT, "run.p2dl")
        record(src, a.record or 20, (1, 1))
    ok = True
    for kind in (["hash", "instret", "clock", "key"] if a.all else [a.kind]):
        ok = run(src, kind, a.frame) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
