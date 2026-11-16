"""Play a recording of the Pico in DoomV, in DoomV's window.

DoomV runs the same machine code the Pico ran on the inputs the Pico
recorded, so you watch the Pico's game again, and every frame is still
checked against the Pico's. At the end (or at a divergence) DoomV holds its
last frame until you close its window. Several files play one after another.

  python demo/play.py                          # the newest recording in build/demo
  python demo/play.py build/demo/corun.p2dl
  python demo/play.py build/demo/*.p2dl
"""
import argparse
import glob
import os
import sys

from _common import OUT, PROGRESS, banner, frames, latest_recording, rel, replay


def play(path):
    data = open(path, "rb").read()
    fr = frames(data)
    if not fr:
        print(f"demo: {rel(path)} has no frames -- skipped")
        return False
    instret = sum(f[3] + f[4] for f in fr)
    banner(f"Playing {rel(path)} in DoomV\n"
           f"  {len(fr)} frames ({len(fr) / 35:.0f} s of DOOM), {instret / 1e9:.2f} billion instructions, "
           "every frame checked against the Pico")
    work = os.path.join(OUT, "play", os.path.splitext(os.path.basename(path))[0])
    matched, total, _, diverged = replay(path, work, hold=True, pace=True, report=show)
    return matched == total and not diverged


def show(matched, total, lines, diverged):
    """The result, printed while DoomV still shows the last frame."""
    if diverged:
        banner("DIVERGED\n" + "\n".join(l for l in lines if not PROGRESS.match(l)))
    else:
        banner(f"all {matched} of {total} frames identical to the Pico's"
               + (" -- stopped early" if matched < total else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("recordings", nargs="*", help=".p2dl files (default: the newest in build/demo)")
    a = ap.parse_args()
    paths = [p for arg in a.recordings for p in (sorted(glob.glob(arg)) or [arg])]
    if not paths:
        newest = latest_recording()
        if not newest:
            sys.exit("demo: no recordings in build/demo -- make one with demo/corun.py or demo/record_replay.py")
        paths = [newest]
    ok = True
    for p in paths:
        if not os.path.isfile(p):
            print(f"demo: no such file {p}")
            ok = False
            continue
        ok = play(p) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
