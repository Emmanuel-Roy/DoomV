#!/usr/bin/env python3
"""Build DoomV once and run regression suites, preserving their real exit status."""
import argparse
from datetime import datetime
import json
import subprocess
import sys

from common import ROOT, BUILD, bash, build_emulator, checkout_lock, entrypoint, environment
from boot_linux import linux_command, smoke_test

SUITES = ("differential", "archtest", "hypervisor", "vector", "riscvtests", "linux")
SLOW = ("vector", "riscvtests")


def suite_command(name):
    tests = ROOT / "tools/verification/tests"
    if name == "differential":
        return [bash(), str(tests / "differential/vector/run_diff.sh")]
    if name == "archtest":
        return [sys.executable, str(tests / "archtest/archtest.py"), "--timeout", "150"]
    directory = {"hypervisor": "damo-tests", "vector": "riscv-vector-tests-v128x64", "riscvtests": "riscv-tests"}[name]
    return [sys.executable, str(tests / "suites/run_suite.py"), directory, "--timeout", "120"]


def run_suite(command, log):
    """Stream complete output once; the process exit code determines the verdict."""
    with log.open("w", encoding="utf-8") as output:
        proc = subprocess.Popen(command, cwd=ROOT, env=environment(), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace")
        try:
            for line in proc.stdout:
                print(line, end="", flush=True)
                output.write(line)
                output.flush()
            return proc.wait()
        finally:
            proc.stdout.close()
            if proc.poll() is None:
                proc.terminate()
                proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suites", nargs="*", help="suite names: " + ", ".join(SUITES))
    parser.add_argument("--quick", action="store_true", help="omit vector and riscvtests from the default selection")
    parser.add_argument("--no-build", action="store_true", help="explicitly test the existing emulator")
    parser.add_argument("--linux-timeout", type=float, default=180)
    args = parser.parse_args()
    if set(args.suites) - set(SUITES):
        parser.error("unknown suite: " + ", ".join(sorted(set(args.suites) - set(SUITES))))
    if args.quick and args.suites:
        parser.error("use --quick or explicit suites, not both")
    if args.linux_timeout <= 0:
        parser.error("--linux-timeout must be positive")
    selected = list(dict.fromkeys(args.suites)) or [s for s in SUITES if not (args.quick and s in SLOW)]
    logs = BUILD / "logs" / datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    logs.mkdir(parents=True)
    results = {}
    with checkout_lock():
        if not args.no_build:
            build_emulator()
        for name in selected:
            print(f"\n=== {name} ===", flush=True)
            try:
                if name == "linux":
                    smoke_test(linux_command(True), logs / "linux.log", args.linux_timeout)
                    code = 0
                else:
                    code = run_suite(suite_command(name), logs / f"{name}.log")
                results[name] = {"status": "pass" if code == 0 else "fail", "exit_code": code}
            except (OSError, RuntimeError) as exc:
                print(f"ERROR: {exc}", file=sys.stderr)
                results[name] = {"status": "error", "detail": str(exc)}
        (logs / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print("\n=== Summary ===")
    for name, result in results.items():
        print(f"{name:14} {result['status'].upper()}")
    print(f"Full logs: {logs}")
    return 0 if results and all(r["status"] == "pass" for r in results.values()) else 1


if __name__ == "__main__":
    sys.exit(entrypoint(main))
