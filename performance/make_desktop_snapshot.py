#!/usr/bin/env python3
"""Make the snapshots bench.py's `session` and `desktop` workloads start from.

Both boot the `ubuntu` workload's machine -- ubuntu.img with the XFCE device
tree, no drives, no shared folder -- from reset and save it:

  session  step 21G, as the XFCE session's programs start up    ~3 minutes
  desktop  step 100G, the desktop up and idle                    ~15 minutes

The screen at that step is written next to the snapshot as <name>.ppm. How to
open the desktop snapshot in a window is in the README, under "Loading the
booted XFCE desktop".

  python performance/make_desktop_snapshot.py             # both
  python performance/make_desktop_snapshot.py session
"""
import argparse
import subprocess
import sys
import time

import bench

SNAPSHOTS = {
    "session": (bench.SESSION_STEP, bench.SESSION_SNAPSHOT),
    "desktop": (bench.DESKTOP_STEP, bench.DESKTOP_SNAPSHOT),
}


def make(name: str):
    step, snap = SNAPSHOTS[name]
    spec = bench.WORKLOADS["ubuntu"]
    snap.parent.mkdir(parents=True, exist_ok=True)
    spec["prepare"]()   # a fresh copy of ubuntu.img; the snapshot takes its own copy
    screen = snap.parent / f"{snap.name}.ppm"
    cmd = [str(bench.ROOT / "riscv_doom.exe")] + spec["args"]() + [
        f"-snapshotat={step}", f"-snapshot={snap}", f"-stopat={step}", f"-fbdump={screen}"]
    print(f"{name}: booting to step {step:,}", flush=True)
    started = time.time()
    r = subprocess.run(cmd, cwd=snap.parent, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode != 0 or not (snap / "state.bin").exists():
        sys.exit(f"{name}: failed (exit {r.returncode}):\n{(r.stdout + r.stderr)[-2000:]}")
    print(f"{name}: saved {snap} in {(time.time() - started) / 60:.0f} minutes; the screen is {screen}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("which", nargs="?", choices=sorted(SNAPSHOTS) + ["both"], default="both")
    args = ap.parse_args()
    for name in (SNAPSHOTS if args.which == "both" else [args.which]):
        make(name)


if __name__ == "__main__":
    main()
