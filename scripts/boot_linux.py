#!/usr/bin/env python3
"""Build and boot Linux with a BusyBox shell.

  python scripts/boot.py linux                  # window; type at the shell
  python scripts/boot.py linux --harts 4        # four CPUs
  python scripts/boot.py linux --smoke          # test: BusyBox runs, then exit
"""
import argparse
from pathlib import Path
import subprocess
import sys
import time

from common import (ROOT, BUILD, add_advanced_options, add_boot_options, boot_args, build_emulator, checkout_lock, entrypoint,
                    environment, hart_dtb, require_files, run, split_passthrough, wsl_script)

SMOKE_MARKER = "DOOMV_USERSPACE_OK"


def linux_command(smoke=False, emulator_args=(), harts=1):
    """The emulator command line for the BusyBox boot, or its smoke-test variant."""
    images = BUILD / "linux"
    dtb = hart_dtb(images / ("smoke.dtb" if smoke else "doomv.dtb"), harts)
    paths = [images / "fw_jump.elf", images / "Image", dtb,
             images / ("smoke.cpio" if smoke else "initramfs.cpio")]
    require_files(ROOT / "riscv_doom.exe", *paths)
    return [str(ROOT / "riscv_doom.exe"), *emulator_args] + [
        f"-{name}={path}" for name, path in zip(("opensbi", "kernel", "dtb", "initrd"), paths)]


def smoke_test(command, log: Path, timeout: float):
    """Pass when the guest prints the marker. Only stops our own child."""
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as output:
        proc = subprocess.Popen(command, cwd=ROOT, env=environment(), stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                text = log.read_text(errors="replace")
                if "Kernel panic" in text:
                    raise RuntimeError(f"Linux panicked; see {log}")
                # Stripped, not equal: the marker arrives through an emulated
                # console and has been seen with a leading space.
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
    argv, passthrough = split_passthrough(sys.argv[1:])
    parser = argparse.ArgumentParser(prog="boot.py linux", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    add_boot_options(parser)
    t = parser.add_argument_group("test")
    t.add_argument("--smoke", action="store_true", help="exit once BusyBox runs a self-check")
    t.add_argument("--timeout", type=float, default=180, metavar="SECONDS", help="--smoke timeout (default 180)")
    add_advanced_options(parser)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    with checkout_lock():
        if not args.no_build:
            build_emulator()
            wsl_script("build_linux.sh")
        if args.smoke:
            smoke_test(linux_command(True, boot_args(args, passthrough), args.harts),
                       BUILD / "logs/linux-smoke.log", args.timeout)
        else:
            if not args.headless:
                print("Linux opens in the emulator window. Type there for the shell; close it to exit.")
            run(linux_command(False, boot_args(args, passthrough), args.harts))
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
