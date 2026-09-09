#!/usr/bin/env python3
"""Run one guest in a private directory and collect a complete signature."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from common import ROOT, environment, require_files, entrypoint


def run_dut(elf, march, begin, end, timeout, *, halt=None, tohost=None):
    if begin % 4 or end <= begin or (end - begin) % 4 or end - begin > 16 * 1024 * 1024:
        raise ValueError("invalid or excessive signature range")
    require_files(ROOT / "riscv_doom.exe", elf, ROOT / "tools/doom/doombuild/DOOM1.WAD")
    command = [str(ROOT / "riscv_doom.exe"), str(ROOT / "tools/doom/doombuild/DOOM1.WAD"),
               str(Path(elf).resolve()), "-march=" + march, f"-sig={begin:x}:{end:x}"]
    if halt is not None:
        command.append(f"-break={halt:x}")
    if tohost is not None:
        command.append(f"-tohost={tohost:x}")
    expected = (end - begin) // 4
    with tempfile.TemporaryDirectory(prefix="doomv-dut-") as directory:
        sig = Path(directory) / "signature.log"
        proc = subprocess.Popen(command, cwd=directory, env=environment(),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if sig.exists():
                    # dump_signature emits 8 hex digits plus LF (or Windows CRLF).
                    data = sig.read_text()
                    if data.endswith("\n") and len(data.splitlines()) == expected:
                        words = data.splitlines()
                        if any(len(word) != 8 for word in words):
                            raise RuntimeError("malformed DUT signature")
                        [int(word, 16) for word in words]
                        return data
                if proc.poll() is not None:
                    raise RuntimeError(f"DoomV exited ({proc.returncode}) without a complete signature")
                time.sleep(0.1)
            raise RuntimeError(f"DoomV did not produce {expected} signature words within {timeout}s")
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
    parser.add_argument("elf", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--march", required=True)
    parser.add_argument("--begin", type=lambda x: int(x, 16), required=True)
    parser.add_argument("--end", type=lambda x: int(x, 16), required=True)
    parser.add_argument("--halt", type=lambda x: int(x, 16), required=True)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    args.output.unlink(missing_ok=True)
    data = run_dut(args.elf, args.march, args.begin, args.end, args.timeout, halt=args.halt)
    args.output.write_text(data)
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
