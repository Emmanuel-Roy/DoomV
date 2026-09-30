#!/usr/bin/env python3
"""Check that a snapshot restores the machine exactly.

For a bench.py workload, three runs to step N+M:

  A  straight through, saving a snapshot at step N on the way
  B  restored from that snapshot
  C  straight through with no snapshot at all

crash.log (registers, CSRs, the clock, the step count, the last 4096
instructions) and DOOMV_STATEDUMP's statedump.log (RAM and both framebuffers)
must be identical from all three: A against C says saving does not disturb the
run, B against A says restoring puts back everything that matters.

  python tools/verification/snapshot_check.py linux
  python tools/verification/snapshot_check.py ubuntu --at 1500000000 --more 500000000
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "performance"))
import bench  # noqa: E402  (the workloads, and the Ubuntu image preparation)

WORK = ROOT / "build" / "snapshot-check"


def run(name: str, exe: pathlib.Path, args: list, extra: list) -> tuple:
    work = WORK / name
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    env = dict(os.environ, DOOMV_STATEDUMP="1")
    t = time.time()
    r = subprocess.run([str(exe.resolve())] + args + extra, cwd=work, env=env,
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    took = time.time() - t
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0 or not (work / "crash.log").exists():
        sys.exit(f"{name}: exit {r.returncode}\n{out[-2000:]}")
    return work, took, out


def files_equal(x: pathlib.Path, y: pathlib.Path) -> bool:
    if x.stat().st_size != y.stat().st_size:
        return False
    with x.open("rb") as fx, y.open("rb") as fy:
        while True:
            bx, by = fx.read(1 << 24), fy.read(1 << 24)
            if bx != by:
                return False
            if not bx:
                return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("workload", choices=sorted(bench.WORKLOADS))
    ap.add_argument("--exe", type=pathlib.Path, default=ROOT / "riscv_doom.exe")
    ap.add_argument("--at", type=int, help="step to snapshot at (default: half the workload)")
    ap.add_argument("--more", type=int, help="steps to run past it (default: the other half)")
    args = ap.parse_args()

    spec = bench.WORKLOADS[args.workload]
    at = args.at or spec["steps"] // 2
    end = at + (args.more or spec["steps"] - at)
    snap = WORK / "snapshot"
    shutil.rmtree(snap, ignore_errors=True)

    def fresh_args():
        if "prepare" in spec:
            spec["prepare"]()   # a fresh copy of the disk image for every run
        return spec["args"]()

    a, ta, _ = run("A-save", args.exe, fresh_args(), [f"-snapshotat={at}", f"-snapshot={snap}", f"-stopat={end}"])
    b, tb, out_b = run("B-restore", args.exe, fresh_args(), [f"-restore={snap}", f"-stopat={end}"])
    c, tc, _ = run("C-straight", args.exe, fresh_args(), [f"-stopat={end}"])

    size = sum(p.stat().st_size for p in snap.iterdir() if p.is_file())
    print(f"{args.workload}: snapshot at {at}, run to {end}; snapshot {size / 2**20:.0f} MB on disk")
    print(f"  A save+run {ta:.1f} s   B restore+run {tb:.1f} s   C straight {tc:.1f} s")
    ok = True
    for f in ("crash.log", "statedump.log"):
        fa, fb, fc = ((d / f).read_bytes() for d in (a, b, c))
        same_ab, same_ac = fa == fb, fa == fc
        print(f"  {f:14} A=B {'yes' if same_ab else 'NO'}   A=C {'yes' if same_ac else 'NO'}")
        ok &= same_ab and same_ac
    # The disk, where there is one: B ran on a working copy of the snapshot's
    # image, C on a fresh copy of the original. What the guest wrote must match.
    work_disk = snap / "disk.work.img"
    if work_disk.exists():
        same = files_equal(work_disk, bench.BENCH_IMAGE)
        print(f"  {'disk image':14} B=C {'yes' if same else 'NO'}")
        ok &= same
    if not ok:
        print(f"  compare the files under {WORK}")
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
