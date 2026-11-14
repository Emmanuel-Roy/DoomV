"""Demo 1: co-simulation. The Pico and DoomV run DOOM together, live.

The Pico plays (you, on the PC keyboard in the Pico's window); everything it
reads goes over USB to DoomV, which runs the same machine code on the same
inputs and checks every frame as it arrives: the picture, the game state and
the number of instructions retired. The Pico is never more than --ahead
frames in front. At the first difference both stop on that frame.

  python demo/corun.py                  # E1M1, 60 s, both windows
  python demo/corun.py --seconds 0      # until Ctrl+C
  python demo/corun.py --headless       # DoomV without its window
"""
import argparse
import os
import subprocess
import sys

from _common import OUT, PICOLINK, banner, need_guest, rel


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=float, default=60, help="how long (0: until Ctrl+C)")
    ap.add_argument("--warp", nargs=2, type=int, default=[1, 1], metavar=("E", "M"), help="map (default E1M1)")
    ap.add_argument("--headless", action="store_true", help="DoomV without its window")
    ap.add_argument("--ahead", type=int, default=2, help="frames the Pico may run ahead of DoomV")
    a = ap.parse_args()
    need_guest()
    out = os.path.join(OUT, "corun.p2dl")
    os.makedirs(OUT, exist_ok=True)
    banner("Co-simulation: Pico 2 (real RISC-V silicon) + DoomV (emulator), live\n"
           f"  map E{a.warp[0]}M{a.warp[1]}, the Pico at most {a.ahead} frames ahead of DoomV\n"
           "  click the Pico window and play: arrows move, Ctrl fires, Space opens doors")
    cmd = [sys.executable, "-u", PICOLINK, "corun", out, "--warp", str(a.warp[0]), str(a.warp[1]),
           "--ahead", str(a.ahead)]
    if a.seconds:
        cmd += ["--seconds", str(a.seconds)]
    if a.headless:
        cmd.append("--headless")
    # picolink prints DoomV's per-frame acks too; the demo shows the rest.
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    try:
        for line in p.stdout:
            if line.startswith("picolink-ack") or line.startswith("input:") or not line.strip():
                continue
            print(line.rstrip())
    except KeyboardInterrupt:
        pass
    p.wait()
    print(f"\ndemo: the recording is in {rel(out)} -- replay it with demo/record_replay.py --replay {rel(out)}")
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
