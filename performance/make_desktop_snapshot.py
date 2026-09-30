#!/usr/bin/env python3
"""Make the snapshot bench.py's `desktop` workload starts from.

Boots the `ubuntu` workload's machine -- ubuntu.img with the XFCE device tree,
no drives, no shared folder -- from reset to step 100G, where the XFCE desktop
is up and idle, and saves it to build/desktop/xfce-100G. About 25 minutes, at
the ~70 MIPS the boot averages. The screen at that step is written to
build/desktop/screen.ppm, to see that it is the desktop. How to open it in a
window is in the README, under "Loading the booted XFCE desktop".

  python performance/make_desktop_snapshot.py
"""
import subprocess
import sys
import time

import bench


def main():
    spec = bench.WORKLOADS["ubuntu"]
    step, snap = bench.DESKTOP_STEP, bench.DESKTOP_SNAPSHOT
    work = snap.parent
    work.mkdir(parents=True, exist_ok=True)
    spec["prepare"]()   # a fresh copy of ubuntu.img; the snapshot takes its own copy
    cmd = [str(bench.ROOT / "riscv_doom.exe")] + spec["args"]() + [
        f"-snapshotat={step}", f"-snapshot={snap}", f"-stopat={step}", f"-fbdump={work / 'screen.ppm'}"]
    print(f"booting to step {step:,}; this takes about 25 minutes", flush=True)
    started = time.time()
    r = subprocess.run(cmd, cwd=work, stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode != 0 or not (snap / "state.bin").exists():
        sys.exit(f"failed (exit {r.returncode}):\n{(r.stdout + r.stderr)[-2000:]}")
    print(f"saved {snap} in {(time.time() - started) / 60:.0f} minutes; the screen is {work / 'screen.ppm'}")


if __name__ == "__main__":
    main()
