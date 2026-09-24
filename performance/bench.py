#!/usr/bin/env python3
"""Measure DoomV's speed on a fixed workload, and record every run.

Each run executes a workload to an exact step count (-stopat), headless, and
records it under performance/runs/<time>-<workload>-<label>/:

  run.json        commit, emulator hash, workload, steps, seconds, MIPS, and
                  the sha256 of the crash.log the run stopped with
  histogram.txt   with --profile: where the host's time went, per function
                  (see sampler.py)

and appends a line to performance/RUNS.md. The crash.log hash is the
equivalence check: -stopat leaves the machine's whole state and its last 4096
instructions in that file, so two emulators that stop with the same hash
executed the workload identically. An optimization is kept only if its hash
matches the baseline's -- speed is allowed to change, behaviour is not.

  python performance/bench.py linux --profile --label baseline
  python performance/bench.py doom --exe build/perf-baseline/riscv_doom.exe --label baseline
  python performance/bench.py linux --compare build/perf-baseline/riscv_doom.exe --label "interrupt check"
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RUNS = HERE / "runs"
TABLE = HERE / "RUNS.md"
sys.path.insert(0, str(HERE))

LINUX = ROOT / "build" / "linux"
DOOM = ROOT / "tools" / "doom" / "doombuild"

WORKLOADS = {
    # The initramfs boot: OpenSBI, the kernel and BusyBox's shell, which it
    # reaches well before this many steps. Headless, no disk, so nothing on
    # the host can change it.
    "linux": dict(steps=300_000_000, args=lambda: [
        "-ng", f"-opensbi={LINUX / 'fw_jump.elf'}", f"-kernel={LINUX / 'Image'}",
        f"-dtb={LINUX / 'doomv.dtb'}", f"-initrd={LINUX / 'initramfs.cpio'}"]),
    # DOOM's startup and attract-mode demo: the renderer, the WAD reads and
    # the framebuffer, a different mix from the kernel's.
    "doom": dict(steps=1_000_000_000, args=lambda: [
        "-ng", str(DOOM / "DOOM1.WAD"), str(DOOM / "doomv-free.elf")]),
    # Ubuntu 24.04 booting systemd on the XFCE configuration: a real
    # distribution's init, its udev coldplug and its service dependency graph,
    # off a virtio disk. The heaviest of the three by some way -- around 55
    # MIPS where DOOM runs at 80 -- because it is the one doing real MMU work
    # against a real userland rather than a single static binary.
    #
    # It boots *toward* the desktop and does not reach it: X is twenty minutes
    # of emulated time away, which no benchmark can wait for. What this
    # measures is the boot, on the device tree the desktop uses. Benchmarking
    # the running desktop would need a way to resume from a booted machine,
    # which this emulator does not have.
    #
    # `optional`: it needs ubuntu.img, which is built over hours and is not in
    # a fresh checkout, so the things that sweep every workload skip it rather
    # than failing. Name it explicitly to run it.
    "ubuntu": dict(steps=3_000_000_000, optional=True, prepare=lambda: prepare_ubuntu(),
                   args=lambda: [
        "-ng", f"-opensbi={LINUX / 'fw_jump.elf'}", f"-kernel={LINUX / 'Image'}",
        f"-dtb={LINUX / 'ubuntu-xfce.dtb'}", f"-disk={BENCH_IMAGE}",
        # No drives or shared folder: both default to a directory in the
        # working tree, and a benchmark must not depend on what is in them.
        "-drives=", "-shared="]),
}

UBUNTU_IMAGE = ROOT / "ubuntu.img"
BENCH_IMAGE = ROOT / "build" / "bench-ubuntu.img"


def prepare_ubuntu():
    """A fresh copy of the image before every run.

    The root disk is opened read-write, so a run writes to it -- systemd's
    journal, the random seed, whatever else early boot touches. Benchmarking
    the real ubuntu.img would therefore mean each run started from what the
    last one left, which is neither reproducible nor kind to an image that
    took hours to build. About two seconds for 4GB, and outside the timed
    section.
    """
    if not UBUNTU_IMAGE.exists():
        raise RuntimeError(f"{UBUNTU_IMAGE} does not exist; see tools/linux/ubuntu/README.md")
    BENCH_IMAGE.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(UBUNTU_IMAGE, BENCH_IMAGE)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git(*args) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def slug(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", text).strip("-").lower()[:40] or "run"


def run_once(workload: str, exe: Path, steps: int, label: str, profile: bool, interval: float) -> dict:
    spec = WORKLOADS[workload]
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = RUNS / f"{stamp}-{workload}-{slug(label)}"
    # The record is written only for a run that finished; a failed run leaves
    # its console log in build/perf-work and nothing in runs/.
    work = ROOT / "build" / "perf-work" / out.name
    work.mkdir(parents=True, exist_ok=True)

    if spec.get("prepare"):
        spec["prepare"]()
    cmd = [str(exe.resolve())] + spec["args"]() + [f"-stopat={steps}"]
    started = time.perf_counter()
    with (work / "console.log").open("wb") as console:
        proc = subprocess.Popen(cmd, cwd=work, stdin=subprocess.DEVNULL, stdout=console, stderr=subprocess.STDOUT)
        sampler = None
        if profile:
            from sampler import Sampler
            sampler = Sampler(proc.pid, exe.resolve(), interval)
            sampler.start()
        code = proc.wait()
        seconds = time.perf_counter() - started
        if sampler:
            sampler.stop()

    crash = work / "crash.log"
    if code != 0 or not crash.exists():
        raise RuntimeError(f"{workload} run failed (exit {code}); see {work / 'console.log'}")
    out.mkdir(parents=True)
    record = {
        "time": stamp,
        "label": label,
        "workload": workload,
        "steps": steps,
        "seconds": round(seconds, 3),
        "mips": round(steps / seconds / 1e6, 3),
        "crash_log_sha256": sha256(crash),
        "exe": str(exe),
        "exe_sha256": sha256(exe),
        "commit": git("rev-parse", "--short", "HEAD"),
        "dirty": bool(git("status", "--porcelain", "--untracked-files=no", "--", "src", "Makefile")),
        "host": f"{platform.processor() or platform.machine()} / {platform.system()} {platform.release()}",
        "profiled": profile,
    }
    if sampler:
        from sampler import render
        hist = sampler.histogram()
        (out / "histogram.txt").write_text(
            f"{workload}, {steps:,} steps, {record['mips']} MIPS while sampled "
            f"(sampling every {interval * 1000:g} ms slows the run a little)\n\n"
            + render(hist, sampler.samples), encoding="utf-8")
        record["samples"] = sampler.samples
        record["top"] = [[n, c] for n, c in hist[:15]]
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    append_table(record, out)
    shutil.rmtree(work, ignore_errors=True)
    return record


def append_table(r: dict, out: Path):
    if not TABLE.exists():
        TABLE.write_text(
            "# Recorded runs\n\n"
            "Written by `bench.py`, one line per run. Runs with the same workload and step count\n"
            "must have the same crash.log hash: that is the proof an optimization changed speed\n"
            "and nothing else.\n\n"
            "| time | workload | label | commit | MIPS | seconds | crash.log | profile |\n"
            "|---|---|---|---|---:|---:|---|---|\n", encoding="utf-8")
    commit = r["commit"] + ("+" if r["dirty"] else "")
    prof = f"[histogram](runs/{out.name}/histogram.txt)" if r["profiled"] else ""
    with TABLE.open("a", encoding="utf-8") as f:
        f.write(f"| {r['time']} | {r['workload']} {r['steps'] // 1_000_000}M | {r['label']} | {commit} | "
                f"{r['mips']:.2f} | {r['seconds']:.1f} | `{r['crash_log_sha256'][:12]}` | {prof} |\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("workload", choices=sorted(WORKLOADS))
    ap.add_argument("--exe", type=Path, default=ROOT / "riscv_doom.exe")
    ap.add_argument("--steps", type=int, help="override the workload's step count")
    ap.add_argument("--label", default="", help="what this run is, for the record")
    ap.add_argument("--profile", action="store_true", help="sample the CPU thread and write a histogram")
    ap.add_argument("--interval", type=float, default=0.002, help="sampling interval in seconds")
    ap.add_argument("--compare", type=Path, metavar="EXE",
                    help="also run this emulator (usually the baseline) and require the same crash.log")
    args = ap.parse_args()
    steps = args.steps or WORKLOADS[args.workload]["steps"]

    runs = []
    if args.compare:
        runs.append(run_once(args.workload, args.compare, steps, f"{args.label} (reference)".strip(), False, args.interval))
    runs.append(run_once(args.workload, args.exe, steps, args.label, args.profile, args.interval))
    for r in runs:
        print(f"{r['label'] or '-':30} {r['mips']:7.2f} MIPS  {r['seconds']:7.1f} s  crash.log {r['crash_log_sha256'][:12]}")
    if args.compare:
        ref, new = runs
        same = ref["crash_log_sha256"] == new["crash_log_sha256"]
        print(f"speedup {new['mips'] / ref['mips']:.3f}x; crash.log {'IDENTICAL' if same else 'DIFFERENT'}")
        return 0 if same else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
