#!/usr/bin/env python3
"""DoomV's machine parameters, each held to Sail set the same way.

The architectural parameters that are not extensions -- the PMP's entries
and grain, ASID and VMID widths, the physical address width, the
cache-block size, whether misaligned loads and stores trap -- are DoomV
switches (-pmp, -pmp-grain, -asidlen, -vmidlen, -physaddr-bits, -cbo-block,
-misaligned), so a core built with other values can be held to a DoomV built
the same. tests/lockstep/switches/machine_params.S probes each; this runs it
on Sail with its configuration set to a value and on DoomV with the switch,
in strict lock-step, for the defaults (Sail's) and each case below. Every
run has to match Sail record for record, and every case has to matter:
DoomV left at the defaults, against Sail set otherwise, has to stop at a
mismatch.

    python tools/verification/machine_params.py
"""
import copy
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "verification"))
import lockstep_sail  # noqa: E402  (Sail's runner, its flags and config)

run_suite = lockstep_sail.run_suite
WORK = ROOT / "build" / "machine-params"
SOURCE = ROOT / "tools" / "verification" / "tests" / "lockstep" / "switches" / "machine_params.S"
TOOLCHAIN_MARCH = "rv64imafdch_zicsr_zicboz"


def pmp(count, usable):
    def f(c):
        c["memory"]["pmp"]["count"] = count
        c["memory"]["pmp"]["usable_count"] = usable
    return f


def grain(g):
    def f(c):
        c["memory"]["pmp"]["grain"] = g
        c["memory"]["pmp"]["na4_supported"] = g == 0
    return f


def key(section, name, value):
    def f(c):
        c[section][name] = value
    return f


def cbo(size):
    def f(c):
        c["platform"]["cache_block_size_exp"] = size.bit_length() - 1
        c["extensions"]["Zic64b"]["supported"] = size == 64
    return f


def misaligned_trap(c):
    c["memory"]["misaligned"]["exceptions"]["load_store"] = {"Some": "AlignmentException"}


# name: (DoomV's switches, what changes in Sail's configuration)
CASES = {
    "pmp 0": (["-pmp=0"], pmp(0, 0)),
    "pmp 64": (["-pmp=64"], pmp(64, 64)),
    "pmp 16, 8 usable": (["-pmp=16:8"], pmp(16, 8)),
    "pmp grain 1": (["-pmp-grain=1"], grain(1)),
    "pmp grain 3": (["-pmp-grain=3"], grain(3)),
    "asidlen 0": (["-asidlen=0"], key("memory", "asidlen", 0)),
    "asidlen 9": (["-asidlen=9"], key("memory", "asidlen", 9)),
    "vmidlen 0": (["-vmidlen=0"], key("memory", "vmidlen", 0)),
    "vmidlen 7": (["-vmidlen=7"], key("memory", "vmidlen", 7)),
    "physaddr 40 bits": (["-physaddr-bits=40"], key("memory", "physaddr_bits", 40)),
    "cbo block 32": (["-cbo-block=32"], cbo(32)),
    "cbo block 128": (["-cbo-block=128"], cbo(128)),
    "misaligned trap": (["-misaligned=trap"], misaligned_trap),
}


def one(name, switches, change, elf, tohost):
    work = WORK / name.replace(" ", "-").replace(",", "")
    work.mkdir(parents=True, exist_ok=True)
    cfg = json.loads(lockstep_sail.SAIL_CONFIG.read_text())
    if change:
        change(cfg)
    config = work / "sail.json"
    config.write_text(json.dumps(cfg, indent=2))
    trace = work / "sail.log"
    r = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", run_suite.WSL_SAIL, "--config",
                        lockstep_sail.wsl(config)] + lockstep_sail.SAIL_FLAGS
                       + ["--trace-output", lockstep_sail.wsl(trace), lockstep_sail.wsl(elf)],
                       capture_output=True, text=True, timeout=300, env=run_suite._env())
    said = (r.stdout or "") + (r.stderr or "")
    if "SUCCESS" not in said:
        return False, "Sail did not finish: " + said.strip()[-800:]

    def doomv(extra):
        d = subprocess.run([str(ROOT / "riscv_doom.exe"), "-ng", str(ROOT / "tools/doom/doombuild/DOOM1.WAD"),
                            str(elf), "-march=" + run_suite.SUITE_MARCH, f"-tohost={tohost:x}"] + extra
                           + [f"-lockstep={trace}", "-lockstep-strict", "-stopat=1000000"],
                           cwd=work, capture_output=True, text=True, timeout=300)
        return (d.stdout or "") + (d.stderr or "")

    out = doomv(switches)
    if "MISMATCH" in out:
        return False, out[out.index("lockstep: MISMATCH"):].strip()[:2500]
    if switches and "MISMATCH" not in doomv([]):
        return False, "DoomV at the defaults matches Sail set this way: the test does not see the parameter"
    verdict = next((l for l in out.splitlines() if l.startswith("lockstep: ")), "no verdict: " + out[-400:])
    ok = ("all matching" in verdict or "matched before the run ended" in verdict) and \
         (work / "tohost.log").exists() and (work / "tohost.log").read_text().strip() == "1"
    traps = trace.read_text(errors="replace").count("handling exc#")
    return ok, f"{verdict.removeprefix('lockstep: ')}; {traps} traps"


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    elf = WORK / "machine_params"
    r = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", "riscv64-unknown-elf-gcc",
                        "-march=" + TOOLCHAIN_MARCH, "-mabi=lp64d", "-nostdlib", "-static", "-Wl,-N",
                        "-Wl,-Ttext=0x80000000", "-Wl,--no-relax", "-o", lockstep_sail.wsl(elf),
                        lockstep_sail.wsl(SOURCE)], capture_output=True, text=True, env=run_suite._env())
    if r.returncode:
        print("cannot assemble the test: " + r.stderr)
        return 1
    tohost = run_suite.elf_symbols(elf)["tohost"]
    failed = 0
    for name, (switches, change) in {"defaults": ([], None), **CASES}.items():
        ok, detail = one(name, switches, change, elf, tohost)
        failed += not ok
        print(f"{'pass' if ok else 'FAIL'} {name}: " + detail.replace("\n", "\n    "), flush=True)
    print("\nall pass" if not failed else f"\n{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
