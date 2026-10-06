#!/usr/bin/env python3
"""Lock-step DoomV's Linux machine against Sail, every instruction, devices and all.

Sail's model has a hart, RAM and a CLINT, and none of DoomV's other devices.
So this runs the machine Sail can follow -- the busybox Linux, booted with no
AIA (the APLIC delivering straight to the hart's SEIP: build/linux/
doomv-sail.dtb) and the ISA Sail's configuration has -- and lets Sail take
what DoomV's devices did from DoomV itself:

  1. DoomV runs the machine, writing -cosim-log: every load and store it made
     to a device, every write a device made to RAM, every change of the
     external-interrupt line, each at its step.
  2. DoomV writes out the machine's architectural state where the run starts
     (-export-state) -- at reset, or a snapshot's -- and corun.py's restore
     program builds the ELF Sail starts from.
  3. Sail (sail_riscv_mh --cosim, tools/verification/simulators/sail/multihart)
     runs it. Device accesses are answered from the log, in order, and
     checked: an access DoomV did not make, or a store of another value,
     stops it. The line and the DMA are applied at their steps. Everything
     else is Sail's own.
  4. DoomV runs again, under -lockstep-strict against Sail's trace, streamed
     into it as Sail writes it -- a boot's trace is hundreds of gigabytes.

    python tools/verification/lockstep_linux.py                       # the boot, first 200M instructions
    python tools/verification/lockstep_linux.py --instructions 1300000000   # to the shell
    python tools/verification/lockstep_linux.py --snapshot snap/linux --instructions 1000000
    python tools/verification/lockstep_linux.py --instructions 1300000000 --segment 100000000 [--resume]

On a mismatch, a DoomV snapshot is taken one step before the instruction that
went wrong, as corun.py does, and the line to restore it is printed.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "verification"))
import corun  # noqa: E402  (the restore program, WSL helpers, Sail's config)

LINUX = ROOT / "build" / "linux"
WORK = ROOT / "build" / "lockstep-linux"
SAIL_MH = corun.wsl(ROOT / "tools" / "verification" / "simulators" / "sail" / "multihart" / "build" / "cmake"
                    / "c_emulator" / "sail_riscv_mh")
SAIL_FLAGS = corun.SAIL_FLAGS


def sail_machine() -> list:
    """DoomV's arguments for the machine Sail can follow."""
    march = "_".join(dict.fromkeys(corun.run_suite.SUITE_MARCH.split("_")))
    return ["-ng", f"-opensbi={LINUX / 'fw_jump.elf'}", f"-kernel={LINUX / 'Image'}",
            f"-dtb={LINUX / 'doomv-sail.dtb'}", f"-initrd={LINUX / 'initramfs.cpio'}",
            f"-march={march}", "-drives=", "-shared="]


def cosim_config(out: pathlib.Path) -> pathlib.Path:
    """Sail's configuration with DoomV's memory map: its devices' addresses as
    I/O (answered from the log), the framebuffer as memory, and Sail's own
    test interrupt generator off -- the APLIC is where it would be."""
    cfg = json.loads((ROOT / "tools/verification/simulators/sail/rva23s64.json").read_text())
    regions = cfg["memory"]["regions"]
    io = next(r for r in regions if r["attributes"]["mem_type"] == "IOMemory" and r["base"]["value"] == "0x2000000")
    main = next(r for r in regions if r["attributes"]["mem_type"] == "MainMemory")
    low = json.loads(json.dumps(io))
    low["base"]["value"], low["size"]["value"] = "0x100000", "0x1f00000"    # the test device, the RTC
    fb = json.loads(json.dumps(main))
    fb["base"]["value"], fb["size"]["value"] = "0x50000000", "0x1000000"     # the Linux framebuffer
    regions[:] = [r for r in regions if r is not io and r is not main]
    regions += [low, io, fb, main]
    regions.sort(key=lambda r: int(r["base"]["value"], 16))
    cfg["platform"]["simple_interrupt_generator"]["supported"] = False
    path = out / "sail-cosim.json"
    path.write_text(json.dumps(cfg, indent=1))
    return path


def doomv(args_, cwd, timeout):
    # stdin closed: a headless Linux machine reads its console from stdin, and
    # one that inherits a detached session's could wait on it for ever.
    return subprocess.run([str(corun.DOOMV)] + args_, cwd=cwd, capture_output=True, text=True, errors="replace",
                          timeout=timeout, stdin=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instructions", type=int, default=200_000_000, help="how far to go (default 200M)")
    ap.add_argument("--snapshot", type=pathlib.Path,
                    help="start from this snapshot of the machine instead of from reset")
    ap.add_argument("--timeout", type=int, default=24 * 3600, help="seconds the lock-step may take")
    ap.add_argument("--segment", type=int, help="run in segments of this many instructions, each from a "
                                                "snapshot the one before took (see --resume)")
    ap.add_argument("--resume", action="store_true", help="with --segment: carry on after the last segment finished")
    args = ap.parse_args()

    machine = sail_machine()
    if not (LINUX / "doomv-sail.dtb").exists():
        sys.exit("no build/linux/doomv-sail.dtb: run python scripts/build.py")
    start = [f"-restore={args.snapshot.resolve()}"] if args.snapshot else []
    first = 0
    if args.snapshot:
        probe = WORK / "probe"
        shutil.rmtree(probe, ignore_errors=True)
        doomv(machine + start + [f"-export-state={probe}"], WORK, 600)
        first = json.loads((probe / "state.json").read_text())["step"]
    name = args.snapshot.name if args.snapshot else "boot"

    if not args.segment:
        out = WORK / name
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        return 0 if window(out, machine, start, first, args.instructions, args) else 1

    # In segments: each one starts from the DoomV snapshot the one before it
    # took at its end, and a finished one is written down, so a run of hours
    # that is stopped carries on with --resume from the last segment it
    # finished. Sail starts each from DoomV's state there, so state its trace
    # never shows could differ unseen across a boundary; every instruction
    # within a segment is compared as ever.
    root = WORK / (name + "-segments")
    progress = root / "progress.json"
    if args.resume and progress.exists():
        done = json.loads(progress.read_text())
        print(f"resuming after step {done['step']:,}", flush=True)
    else:
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True)
        done = {"step": first, "snapshot": str(args.snapshot.resolve()) if args.snapshot else None}
    end = first + args.instructions
    while done["step"] < end:
        a = done["step"]
        n = min(args.segment, end - a)
        seg_start = [f"-restore={done['snapshot']}"] if done["snapshot"] else []
        snap = root / f"snapshot-{a + n}"
        print(f"== segment {a:,} to {a + n:,}", flush=True)
        if not window(root / f"segment-{a}", machine, seg_start, a, n, args, end_snapshot=snap):
            return 1
        done = {"step": a + n, "snapshot": str(snap)}
        progress.write_text(json.dumps(done))
    print(f"all {args.instructions:,} instructions from step {first:,} match Sail", flush=True)
    return 0


def window(out, machine, start, first, count, args, end_snapshot=None) -> bool:
    """Lock-step `count` instructions from step `first`; True if they all match."""
    out.mkdir(parents=True, exist_ok=True)
    # 1. DoomV's run, and its devices -- and the snapshot the next segment starts from.
    t0 = time.time()
    log = out / "doomv.cosim"
    snap = [f"-snapshot={end_snapshot}", f"-snapshotat={first + count}"] if end_snapshot else []
    r = doomv(machine + start + snap + [f"-cosim-log={log}", f"-stopat={first + count}"], out, args.timeout)
    (out / "doomv-record.txt").write_text((r.stdout or "") + (r.stderr or ""))
    if not log.exists():
        print("DoomV wrote no device log: " + ((r.stdout or "") + (r.stderr or "")).strip()[-500:])
        return False
    if end_snapshot and not (end_snapshot / "state.bin").exists():
        print("DoomV took no snapshot at the segment's end: " + ((r.stdout or "") + (r.stderr or "")).strip()[-300:])
        return False
    counts = {}
    with log.open() as f:
        for line in f:
            counts[line[:1]] = counts.get(line[:1], 0) + 1
    print(f"1. DoomV ran {count:,} instructions in {time.time() - t0:.0f} s: "
          f"{counts.get('R', 0):,} device loads, {counts.get('W', 0):,} device stores, "
          f"{counts.get('M', 0):,} device writes to RAM, {counts.get('I', 0):,} changes of the interrupt line", flush=True)

    # 2. The state it starts from, as an ELF Sail can load.
    state_dir = out / "state"
    r = doomv(machine + start + [f"-export-state={state_dir}"], out, 600)
    if not (state_dir / "state.json").exists():
        print("DoomV could not export the state: " + ((r.stdout or "") + (r.stderr or "")).strip()[-400:])
        return False
    elf, state, _ = corun.build_restore_elf(state_dir, out, 600)
    start_pc = int(state["pc"], 16)
    print(f"2. the machine at step {state['step']:,}, pc {start_pc:#x}: {elf.name}", flush=True)

    # 3 and 4. Sail, its trace streamed into DoomV's lock-step.
    config = cosim_config(out)
    sail_cmd = ["wsl", "-d", "Ubuntu", "-u", "root", "--", SAIL_MH, "--config", corun.wsl(config),
                "--cosim", corun.wsl(log), "--cosim-start", hex(start_pc),
                "--inst-limit", str(count + 4096)] + SAIL_FLAGS + \
               ["--trace-output", "/dev/stdout", corun.wsl(elf)]
    t0 = time.time()
    sail = subprocess.Popen(sail_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=corun.run_suite._env())
    sail_err = []
    threading.Thread(target=lambda: sail_err.append(sail.stderr.read().decode(errors="replace")), daemon=True).start()
    # Straight to the file, with a progress line every 50 million steps: a run
    # of hours that is stopped part-way still says how far it matched.
    lock_log = out / "doomv-lockstep.txt"
    env = dict(os.environ, DOOMV_PROGRESS="50000000")
    with lock_log.open("wb") as lf:
        lock = subprocess.Popen([str(corun.DOOMV)] + machine + start +
                                ["-lockstep=-", "-lockstep-strict", f"-stopat={first + count}"],
                                cwd=out, stdin=sail.stdout, stdout=lf, stderr=subprocess.STDOUT, env=env)
        sail.stdout.close()
        lock.wait(timeout=args.timeout)
    text = lock_log.read_text(errors="replace")
    # DoomV stops at its limit, or at a mismatch, with Sail still writing.
    try:
        sail.wait(timeout=10)
    except subprocess.TimeoutExpired:
        sail.terminate()
        sail.wait(timeout=60)
    time.sleep(0.5)
    (out / "sail.err").write_text("".join(sail_err))
    elapsed = time.time() - t0
    cosim_msgs = [l for l in "".join(sail_err).splitlines() if l.startswith("cosim:")]
    verdict = [l for l in text.splitlines() if l.startswith("lockstep: ") and "skipped" not in l]
    print(f"3. Sail and the lock-step, {elapsed:.0f} s", flush=True)
    for m in cosim_msgs:
        print("   Sail: " + m)
    if "MISMATCH" in text:
        detail = text[text.index("lockstep: MISMATCH"):].strip()
        print("   " + detail.replace("\n", "\n   ")[:3000])
        m = re.search(r"\(instruction (\d+)\)", detail)
        if m and int(m.group(1)) - 1 > first:
            snap_dir = out / "doomv-snapshot"
            at = int(m.group(1)) - 1
            doomv(machine + start + [f"-snapshot={snap_dir}", f"-snapshotat={at}", f"-stopat={at + 1}"], out,
                  args.timeout)
            if (snap_dir / "state.bin").exists():
                print(f"   snapshot before the failing instruction (step {at:,}): {snap_dir}")
                print("   restore: " + " ".join([str(corun.DOOMV)] + machine + [f"-restore={snap_dir}"]))
        return False
    print("   " + (verdict[-1] if verdict else "no lock-step verdict -- see " + str(lock_log)), flush=True)
    return bool(verdict) and not cosim_msgs


if __name__ == "__main__":
    sys.exit(main())
