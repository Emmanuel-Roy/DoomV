#!/usr/bin/env python3
"""The in-process lock-step and the core's clock, held to Sail.

What a core lock-steps against DoomV with -- Ouroboros's, in Vitis HLS
software emulation (its C++ in the same process) and hardware emulation (a
testbench reading what the RTL retired) -- checked against Sail's own traces:

1. Sail runs each test and writes its trace.
2. DoomV lock-steps against it, strictly, as lockstep_sail.py does, and with
   -lockstep-stamp writes it back with a cycle stamp before each record:
   Sail's clock, which ticks once every two steps, counted in cycles of one
   tick each.
3. DoomV lock-steps against the stamped trace with -cycle-clock=1: its clock
   now comes from the stamps alone, and every value Sail's trace holds --
   time and counter reads, interrupts, waits -- has to come out the same.
4. The same through the library (make lockstep-lib, src/doomv_lockstep.h),
   by a testbench program (lockstep_lib/feed.cpp) built with Vitis's own
   MinGW g++ when there is one: the stamped records handed over as text one
   at a time, and as doomv_ls_record structures the testbench builds itself.
5. A stamped trace with one value changed must stop the library at it.
6. With Vitis installed, both of Vitis HLS's emulations: a stand-in for a
   core's retirement port (lockstep_lib/vitis) synthesised, its testbench
   handing what comes out of it to the library -- in C simulation (software
   emulation, the C++ in the testbench's process) and in C/RTL co-simulation
   (hardware emulation, the generated Verilog in XSim) -- on clock.S's
   stamped records. --no-vitis skips it.
7. With Vitis, corun.py --lockstep sw-emu: the stand-in as the device under
   test, with Sail beside it, on a few tests -- every one matching -- and
   once as a core with a bug (DOOMV_LS_STANDIN_FAULT), which has to stop at
   it, with Sail agreeing with DoomV there and the snapshot before it
   verified.

The multi-hart tests follow the reference's order (-lockstep-follow, and in
the library, which always does), leniently: Sail's multi-hart clock counts
rounds, which a core's order does not have.

    python tools/verification/lockstep_lib.py
    python tools/verification/lockstep_lib.py clock rv64si-p-wfi
    python tools/verification/lockstep_lib.py --all     # every test lockstep_sail.py runs

The Vitis compiler: VITIS_GXX, else the first Vitis installation under
Z:/FPGA, C:/Xilinx or C:/AMD; without one, DoomV's own compiler builds the
testbench instead.
"""
import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "verification"))
import lockstep_sail  # noqa: E402  (Sail's runner, the test ELFs)
from vitis_dut import STAND_IN, clean_dir, component_config, vitis_gxx, vitis_root  # noqa: E402

run_suite = lockstep_sail.run_suite
WORK = ROOT / "build" / "lockstep-lib-test"
LIB = ROOT / "build" / "lockstep-lib"
WAD = ROOT / "tools" / "doom" / "doombuild" / "DOOM1.WAD"

# One-hart tests: the clock (time, counters, waits in M and S), traps,
# virtual memory with its fetch faults (one Sail record, two steps), floating
# point, atomics.
DEFAULT_TESTS = ["clock", "rv64ui-p-add", "rv64mi-p-scall", "rv64mi-p-illegal",
                 "rv64si-p-wfi", "rv64ui-v-add", "rv64uf-p-fadd", "rv64ud-p-fmadd", "rv64ua-p-amoadd_d"]
MULTIHART_TESTS = ["smp_atomics", "smp_ipi", "smp_timer"]


def build_feed():
    """The library, and the testbench program linked against it."""
    subprocess.run(["make", "lockstep-lib"], cwd=ROOT, check=True)
    WORK.mkdir(parents=True, exist_ok=True)
    shutil.copy2(LIB / "doomv_lockstep.dll", WORK / "doomv_lockstep.dll")
    gxx = vitis_gxx()
    env = dict(os.environ)
    if gxx:
        cmd = [str(gxx)]
        env["PATH"] = str(gxx.parent) + os.pathsep + env["PATH"]
        print(f"testbench compiler: {gxx} (Vitis)", flush=True)
    else:
        cmd = [subprocess.run(["make", "-s", "--no-print-directory", "print-cxx"], cwd=ROOT, capture_output=True,
                              text=True).stdout.strip() or "g++"]
        print(f"testbench compiler: {cmd[0]} (no Vitis found)", flush=True)
    subprocess.run(cmd + ["-O2", "-std=c++17", str(ROOT / "tools/verification/lockstep_lib/feed.cpp"),
                          "-I" + str(LIB), "-L" + str(LIB), "-ldoomv_lockstep",
                          "-static-libgcc", "-static-libstdc++", "-Wl,-Bstatic", "-lwinpthread", "-Wl,-Bdynamic",
                          "-o", str(WORK / "feed.exe")],
                   check=True, env=env)
    return env


def vitis_check(stamped, elf, timeout):
    """Vitis HLS's software and hardware emulation, each handing the
    retirement port's records to DoomV. [(check, ok, detail)]."""
    vitis = vitis_root()
    if not vitis:
        return [("Vitis", None, "no Vitis installation")]
    work = WORK / "vitis"
    clean_dir(work)
    component_config(STAND_IN, work / "hls_config.cfg")
    syms = run_suite.elf_symbols(elf)
    env = dict(os.environ)
    env["DOOMV_LS_TRACE"] = str(stamped)
    env["DOOMV_LS_ARGS"] = " ".join([str(WAD), str(elf), "-march=" + run_suite.SUITE_MARCH,
                                     "-tohost={:x}".format(syms["tohost"]), "-lockstep-strict", "-cycle-clock=1"])
    env["PATH"] = str(LIB) + os.pathsep + env["PATH"]
    results = []
    stages = [("synthesis", "v++.bat", ["-c", "--mode", "hls"]),
              ("software emulation (C simulation)", "vitis-run.bat", ["--mode", "hls", "--csim"]),
              ("hardware emulation (co-simulation in XSim)", "vitis-run.bat", ["--mode", "hls", "--cosim"])]
    for check, tool, args in stages:
        cmd = ["cmd", "/c", str(vitis / "bin" / tool)] + args + ["--config", str(work / "hls_config.cfg"),
                                                                 "--work_dir", str(work / "component")]
        try:
            code, out = run(cmd, work, timeout * 4, env)
        except subprocess.TimeoutExpired:
            results.append(("Vitis " + check, False, "timed out"))
            return results
        (work / (args[-1].strip("-") + ".log")).write_text(out)
        matched = [l for l in out.splitlines() if l.startswith("retire_tb: ")]
        all_matched = bool(matched) and all("matched DoomV" in l for l in matched)
        if tool.startswith("v++"):
            ok = code == 0
            detail = "the retirement port synthesised" if ok else out.strip()[-1500:]
        elif "--cosim" in args:
            # The testbench runs twice: on the C++, then on what the RTL produced.
            ok = code == 0 and "C/RTL co-simulation finished: PASS" in out and len(matched) == 2 and all_matched
            detail = matched[-1] + ", out of the RTL" if ok else "\n".join(matched) or out.strip()[-1500:]
        else:
            ok = code == 0 and all_matched
            detail = matched[-1] if ok else "\n".join(matched) or out.strip()[-1500:]
        results.append(("Vitis " + check, ok, detail))
        if not ok:
            break
    return results


def corun_check(timeout):
    """corun.py --lockstep sw-emu, as it is meant to be used. [(check, ok, detail)]."""
    corun = [sys.executable, str(ROOT / "tools/verification/corun.py"), "--lockstep", "sw-emu", "--sims", "sail",
             "--suite", "riscv-tests"]
    results = []
    code, out = run(corun + ["rv64ui-p-add", "rv64mi-p-scall", "rv64uf-p-fadd"], ROOT, timeout * 4)
    ok = code == 0 and out.count("matches the core (sw-emu)") == 3 and "lock-step match 3" in out
    results.append(("corun --lockstep sw-emu", ok, "3 tests: the core matches DoomV, and Sail matches the core"
                    if ok else out.strip()[-2000:]))
    env = dict(os.environ, DOOMV_LS_STANDIN_FAULT="200")
    code, out = run(corun + ["rv64ui-p-add"], ROOT, timeout * 4, env)
    ok = (code == 1 and "MISMATCH" in out and "sail      DIVERGES at step 200" in out
          and "verified: restoring it runs the failing instruction next" in out)
    results.append(("corun --lockstep sw-emu, a core with a bug", ok,
                    "stopped at it, Sail with DoomV, the snapshot before it verified" if ok else out.strip()[-2000:]))
    return results


def run(cmd, cwd, timeout, env=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def sail_trace(elf, harts, work, timeout):
    trace = work / "sail.log"
    sail_cmd = [run_suite.WSL_SAIL] if harts == 1 else [lockstep_sail.WSL_SAIL_MH, "--harts", str(harts)]
    r = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--"] + sail_cmd
                       + ["--config", lockstep_sail.wsl(lockstep_sail.SAIL_CONFIG)]
                       + lockstep_sail.SAIL_FLAGS + ["--trace-output", lockstep_sail.wsl(trace), lockstep_sail.wsl(elf)],
                       capture_output=True, text=True, timeout=timeout, env=run_suite._env())
    if "SUCCESS" not in (r.stdout or "") + (r.stderr or ""):
        return None
    return trace


def one(name, elf, harts, feed_env, timeout):
    """[(check, ok, detail)] for one test."""
    work = WORK / elf.name
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    trace = sail_trace(elf, harts, work, timeout)
    if not trace:
        return [("sail", None, "Sail does not pass it")]
    syms = run_suite.elf_symbols(elf)
    machine = [str(WAD), str(elf), "-march=" + run_suite.SUITE_MARCH, "-tohost={:x}".format(syms["tohost"])]
    if harts > 1:
        machine.append(f"-harts={harts}")
    doomv = [str(ROOT / "riscv_doom.exe"), "-ng"]
    feed = [str(WORK / "feed.exe")]
    results = []

    def verdict(check, code, out, want_all=True):
        if "MISMATCH" in out or code != 0:
            at = out.find("lockstep: MISMATCH")
            results.append((check, False, (out[at:] if at >= 0 else out).strip()[-2500:]))
            return False
        line = next((l for l in out.splitlines() if l.startswith(("lockstep: ", "feed: "))), "no verdict")
        # To the end of the trace, or to the test's own end (tohost), as
        # lockstep_sail.py counts a pass.
        tohost = work / "tohost.log"
        ok = (not want_all or "all matching" in line or line.startswith("feed: ")
              or ("matched before the run ended" in line and tohost.exists() and tohost.read_text().strip() == "1"))
        results.append((check, ok, line))
        return ok

    if harts > 1:
        code, out = run(doomv + machine + ["-lockstep=" + str(trace), "-lockstep-follow", "-stopat=100000000"],
                        work, timeout)
        if verdict("follow", code, out, want_all=False):
            code, out = run(feed + ["text", str(trace)] + machine, work, timeout, feed_env)
            verdict("library text, following", code, out)
        return results

    stamped = work / "stamped.log"
    code, out = run(doomv + machine + ["-lockstep=" + str(trace), "-lockstep-strict",
                                       "-lockstep-stamp=" + str(stamped), "-stopat=100000000"], work, timeout)
    if not verdict("Sail, stamping", code, out, want_all=False):
        return results
    code, out = run(doomv + machine + ["-lockstep=" + str(stamped), "-lockstep-strict", "-cycle-clock=1",
                                       "-stopat=100000000"], work, timeout)
    verdict("cycle clock", code, out)
    lib = machine + ["-lockstep-strict", "-cycle-clock=1"]
    for mode in ("text", "struct"):
        code, out = run(feed + [mode, str(stamped)] + lib, work, timeout, feed_env)
        verdict("library " + mode, code, out)

    # One value changed: the library must stop there, and say so.
    lines = stamped.read_text().splitlines()
    target = next((i for i, l in enumerate(lines) if re.match(r"x\d+ <- 0x", l) and i > len(lines) // 2), None)
    if target is not None:
        bad = work / "corrupted.log"
        value = lines[target].split("<- ")[1]
        flipped = "0x" + format(int(value, 16) ^ 1, "016X")
        lines[target] = lines[target].split("<- ")[0] + "<- " + flipped
        bad.write_text("\n".join(lines) + "\n")
        for mode in ("text", "struct"):
            code, out = run(feed + [mode, str(bad)] + lib, work, timeout, feed_env)
            caught = code == 1 and "MISMATCH" in out and flipped in out
            results.append(("library " + mode + " catches a changed value", caught,
                            "stopped at it" if caught else out.strip()[-1500:]))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tests", nargs="*", help="test names (default: a set of one-hart tests and the multi-hart ones)")
    ap.add_argument("--harts", type=int, default=2, help="harts for the multi-hart tests (default 2)")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--all", action="store_true", help="every rv64 riscv-tests test, as lockstep_sail.py runs")
    ap.add_argument("--no-vitis", action="store_true", help="skip Vitis HLS's software and hardware emulation")
    args = ap.parse_args()

    feed_env = build_feed()
    own = lockstep_sail.build_lockstep_tests()
    own.update(lockstep_sail.build_multihart_tests([args.harts]))
    names = args.tests or DEFAULT_TESTS + [f"{t}-h{args.harts}" for t in MULTIHART_TESTS]
    if args.all:
        names = [p.name for p in sorted(lockstep_sail.TESTS.iterdir())
                 if re.match(r"rv64[a-z]+-[pv]-|hypervisor-", p.name) and p.is_file() and not p.suffix]
        names += [n for n in own if n not in names]
    failed = 0
    for name in names:
        elf, harts = own.get(name, (lockstep_sail.TESTS / name, 1))
        for check, ok, detail in one(name, elf, harts, feed_env, args.timeout):
            status = "skip" if ok is None else "pass" if ok else "FAIL"
            failed += ok is False
            print(f"{status} {name}: {check}: " + detail.replace("\n", "\n    "), flush=True)
    if not args.no_vitis:
        stamped = WORK / "clock" / "stamped.log"
        if not stamped.exists():
            one("clock", own["clock"][0], 1, feed_env, args.timeout)
        checks = vitis_check(stamped, own["clock"][0], args.timeout)
        if vitis_root():
            checks += corun_check(args.timeout)
        for check, ok, detail in checks:
            status = "skip" if ok is None else "pass" if ok else "FAIL"
            failed += ok is False
            print(f"{status} clock: {check}: " + detail.replace("\n", "\n    "), flush=True)
    print("\nall pass" if not failed else f"\n{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
