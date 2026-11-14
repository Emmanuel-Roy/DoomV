"""Demo 2: record the Pico, then play the recording back in DoomV.

Step 1 records the real Pico 2: every clock value and key DOOM read, and for
every frame a hash of the picture, a hash of the game state and the number
of instructions retired in game logic and in rendering. Step 2 runs DoomV on
that recording: the same machine code gets the same inputs, and each of its
frames is checked against the Pico's.

  python demo/record_replay.py                     # record 30 s on E1M1, replay
  python demo/record_replay.py --seconds 60
  python demo/record_replay.py --replay build/demo/corun.p2dl   # replay only
"""
import argparse
import os
import sys

from _common import LAST, OUT, PROGRESS, banner, frames, record, rel, replay


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=30, help="how long to record")
    ap.add_argument("--warp", nargs=2, type=int, default=[1, 1], metavar=("E", "M"),
                    help="map (default E1M1); --title records the title screen and attract demo")
    ap.add_argument("--title", action="store_true", help="record the title screen and DOOM's own demos")
    ap.add_argument("--replay", metavar="FILE", help="skip recording: replay this .p2dl")
    ap.add_argument("--headless", action="store_true", help="DoomV without its window (faster)")
    a = ap.parse_args()

    path = a.replay or os.path.join(OUT, "run.p2dl")
    if not a.replay:
        record(path, a.seconds, None if a.title else a.warp)
    data = open(path, "rb").read()
    fr = frames(data)
    if not fr:
        sys.exit(f"demo: {rel(path)} has no frames")
    instret = sum(f[3] + f[4] for f in fr)
    banner(f"Replaying {rel(path)} in DoomV\n"
           f"  {len(fr)} frames, {len(data) / 1024:.0f} KB, {instret / 1e9:.2f} billion retired instructions to match")
    matched, total, _, diverged = replay(path, os.path.join(OUT, "replay"), headless=a.headless, hold=True,
                                         report=lambda *r: show(r, instret))
    return 0 if matched == total and not diverged else 1


def show(result, instret):
    """The result, printed while DoomV still shows the last frame."""
    matched, total, lines, diverged = result
    dt = LAST["seconds"]
    if diverged:
        banner("RESULT: DoomV and the Pico DIVERGED\n" + "\n".join(l for l in lines if not PROGRESS.match(l)))
        return
    banner(f"RESULT: all {matched} of {total} frames identical (picture, game state, retired instructions)\n"
           f"  {instret:,} instructions of DOOM retired the same on the Pico and in DoomV\n"
           f"  DoomV replayed {total} frames in {dt:.1f} s ({total / dt:.0f} fps)")


if __name__ == "__main__":
    sys.exit(main())
