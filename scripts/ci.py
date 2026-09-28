#!/usr/bin/env python3
"""The gate: everything that has to pass before a change is allowed in.

One command, one exit code, so that the pre-push hook and the CI workflow
run exactly the same thing and cannot drift apart:

  1. a clean build of the emulator, as `make` would do it
  2. strict lock-step against Sail -- every test, 0 values taken from the
     reference
  3. scripts/verify.py -- differential, archtest, hypervisor, vector,
     riscvtests and linux

Around thirteen minutes on a warm checkout, nearly all of it in step 3.

  python scripts/ci.py              # the whole gate
  python scripts/ci.py --quick      # skip the two slowest suites
  python scripts/ci.py --no-build   # test the emulator already built

Exit code is 0 only if every stage passed, which is the whole point: a
non-zero exit is what the hook and the workflow act on.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

from common import ROOT, build_emulator, environment, entrypoint

LOCKSTEP = ROOT / "tools/verification/lockstep_sail.py"
VERIFY = ROOT / "scripts/verify.py"


def stage(name: str, command: list[str] | None) -> tuple[str, float, int]:
    """Run one stage, streaming its output, and time it."""
    print(f"\n{'=' * 62}\n  {name}\n{'=' * 62}", flush=True)
    start = time.monotonic()
    if command is None:                    # the build, which is a function call
        try:
            build_emulator()
            code = 0
        except Exception as exc:           # noqa: BLE001 -- report, do not raise
            print(f"ERROR: {exc}", file=sys.stderr)
            code = 1
    else:
        code = subprocess.run(command, cwd=ROOT, env=environment()).returncode
    return name, time.monotonic() - start, code


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true",
                    help="pass --quick to verify.py (omits vector and riscvtests)")
    ap.add_argument("--no-build", action="store_true", help="test the emulator already built")
    args = ap.parse_args()

    verify = [sys.executable, str(VERIFY), "--no-build"]
    if args.quick:
        verify.append("--quick")

    stages = []
    if not args.no_build:
        stages.append(("build", None))
    stages.append(("lock-step vs Sail", [sys.executable, str(LOCKSTEP)]))
    stages.append(("verify.py", verify))

    results = []
    for name, command in stages:
        result = stage(name, command)
        results.append(result)
        # Stop at the first failure: the later stages test the same binary, so
        # their output would be noise on top of a failure already found.
        if result[2] != 0:
            break

    print(f"\n{'=' * 62}\n  Gate\n{'=' * 62}")
    for name, seconds, code in results:
        print(f"  {'PASS' if code == 0 else 'FAIL'}  {name:22} {seconds / 60:5.1f} min")
    skipped = len(stages) - len(results)
    if skipped:
        print(f"  ....  {skipped} stage(s) not reached")
    total = sum(seconds for _, seconds, _ in results)
    failed = [name for name, _, code in results if code != 0]
    print(f"\n  {total / 60:.1f} min total")
    if failed:
        print(f"  GATE FAILED: {', '.join(failed)}")
        return 1
    print("  GATE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(entrypoint(main))
