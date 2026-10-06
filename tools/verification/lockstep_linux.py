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

On a mismatch, a DoomV snapshot is taken one step before the instruction that
went wrong, as corun.py does, and the line to restore it is printed.
"""
from __future__ import annotations

import argparse
import json
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
    args = ap.parse_args()

    out = WORK / (args.snapshot.name if args.snapshot else "boot")
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    machine = sail_machine()
    start = [f"-restore={args.snapshot.resolve()}"] if args.snapshot else []
    if not (LINUX / "doomv-sail.dtb").exists():
        sys.exit("no build/linux/doomv-sail.dtb: run python scripts/build.py")

    # 1. DoomV's run, and its devices.
    t0 = time.time()
    log = out / "doomv.cosim"
    first = 0
    if args.snapshot:
        st = doomv(machine + start + [f"-export-state={out / 'probe'}"], out, 600)
        first = json.loads((out / "probe" / "state.json").read_text())["step"]
    r = doomv(machine + start + [f"-cosim-log={log}", f"-stopat={first + args.instructions}"], out, args.timeout)
    (out / "doomv-record.txt").write_text((r.stdout or "") + (r.stderr or ""))
    if not log.exists():
        sys.exit("DoomV wrote no device log: " + ((r.stdout or "") + (r.stderr or "")).strip()[-500:])
    counts = {}
    with log.open() as f:
        for line in f:
            counts[line[:1]] = counts.get(line[:1], 0) + 1
    print(f"1. DoomV ran {args.instructions:,} instructions in {time.time() - t0:.0f} s: "
          f"{counts.get('R', 0):,} device loads, {counts.get('W', 0):,} device stores, "
          f"{counts.get('M', 0):,} device writes to RAM, {counts.get('I', 0):,} changes of the interrupt line", flush=True)

    # 2. The state it starts from, as an ELF Sail can load.
    state_dir = out / "state"
    r = doomv(machine + start + [f"-export-state={state_dir}"], out, 600)
    if not (state_dir / "state.json").exists():
        sys.exit("DoomV could not export the state: " + ((r.stdout or "") + (r.stderr or "")).strip()[-400:])
    elf, state, _ = corun.build_restore_elf(state_dir, out, 600)
    start_pc = int(state["pc"], 16)
    print(f"2. the machine at step {state['step']:,}, pc {start_pc:#x}: {elf.name}", flush=True)

    # 3 and 4. Sail, its trace streamed into DoomV's lock-step.
    config = cosim_config(out)
    sail_cmd = ["wsl", "-d", "Ubuntu", "-u", "root", "--", SAIL_MH, "--config", corun.wsl(config),
                "--cosim", corun.wsl(log), "--cosim-start", hex(start_pc),
                "--inst-limit", str(args.instructions + 4096)] + SAIL_FLAGS + \
               ["--trace-output", "/dev/stdout", corun.wsl(elf)]
    t0 = time.time()
    sail = subprocess.Popen(sail_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=corun.run_suite._env())
    sail_err = []
    threading.Thread(target=lambda: sail_err.append(sail.stderr.read().decode(errors="replace")), daemon=True).start()
    lock = subprocess.Popen([str(corun.DOOMV)] + machine + start +
                            ["-lockstep=-", "-lockstep-strict", f"-stopat={first + args.instructions}"],
                            cwd=out, stdin=sail.stdout, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sail.stdout.close()
    text = lock.communicate(timeout=args.timeout)[0].decode(errors="replace")
    (out / "doomv-lockstep.txt").write_text(text)
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
            snap = out / "doomv-snapshot"
            at = int(m.group(1)) - 1
            doomv(machine + start + [f"-snapshot={snap}", f"-snapshotat={at}", f"-stopat={at + 1}"], out, args.timeout)
            if (snap / "state.bin").exists():
                print(f"   snapshot before the failing instruction (step {at:,}): {snap}")
                print("   restore: " + " ".join([str(corun.DOOMV)] + machine + [f"-restore={snap}"]))
        return 1
    print("   " + (verdict[-1] if verdict else "no lock-step verdict -- see " + str(out / "doomv-lockstep.txt")))
    return 0 if verdict and not cosim_msgs else 1


if __name__ == "__main__":
    sys.exit(main())
