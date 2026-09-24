#!/usr/bin/env python3
"""Build and boot Linux, or prove BusyBox userspace executes with --smoke."""
import argparse
from pathlib import Path
import subprocess
import sys
import time

from common import ROOT, BUILD, add_ram_option, build_emulator, checkout_lock, entrypoint, environment, ram_args, require_files, run, wsl_script

SMOKE_MARKER = "DOOMV_USERSPACE_OK"


def linux_command(smoke=False, ram=None):
    images = BUILD / "linux"
    paths = [images / "fw_jump.elf", images / "Image",
             images / ("smoke.dtb" if smoke else "doomv.dtb"),
             images / ("smoke.cpio" if smoke else "initramfs.cpio")]
    require_files(ROOT / "riscv_doom.exe", *paths)
    # The device tree names a size too, and the emulator rewrites it in the
    # loaded blob to match -ram=, so the guest is told what was allocated.
    return [str(ROOT / "riscv_doom.exe")] + ram_args(ram) + [
        f"-{name}={path}" for name, path in zip(("opensbi", "kernel", "dtb", "initrd"), paths)]


def smoke_test(command, log: Path, timeout: float):
    """Only stop our own child. A kernel 'Run /bin/sh' line is not a pass."""
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as output:
        proc = subprocess.Popen(command, cwd=ROOT, env=environment(), stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                text = log.read_text(errors="replace")
                if "Kernel panic" in text:
                    raise RuntimeError(f"Linux panicked; see {log}")
                # Compared stripped, not for equality. The marker reaches
                # this log through an emulated serial console, and what
                # arrives is not always the bare word: a run that otherwise
                # passed has been seen printing " DOOMV_USERSPACE_OK",
                # with a leading space, and an exact match then misses it and
                # waits out the whole timeout. That failure looks like a
                # broken guest and is not one -- the guest had already
                # printed it -- so it is worth not being fragile about.
                if any(line.strip() == SMOKE_MARKER for line in text.splitlines()):
                    print(f"PASS: BusyBox executed the userspace smoke script. Log: {log}")
                    return
                if proc.poll() is not None:
                    raise RuntimeError(f"Emulator exited before userspace completed; see {log}")
                time.sleep(0.25)
            raise RuntimeError(f"Linux userspace did not complete within {timeout:g}s; see {log}")
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-build", action="store_true", help="use images already in build/linux")
    parser.add_argument("--smoke", action="store_true", help="exit after BusyBox executes a self-check")
    parser.add_argument("--timeout", type=float, default=180, help="smoke timeout in seconds")
    add_ram_option(parser)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    with checkout_lock():
        if not args.no_build:
            build_emulator()
            wsl_script("build_linux.sh")
        if args.smoke:
            smoke_test(linux_command(True, args.ram), BUILD / "logs/linux-smoke.log", args.timeout)
        else:
            print("Linux opens in the emulator window. Type there for the shell; close it to exit.")
            run(linux_command(ram=args.ram))
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
