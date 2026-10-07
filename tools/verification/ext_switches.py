#!/usr/bin/env python3
"""DoomV's extension switches, each held to Sail with it on and off.

DoomV switches the vector unit's extensions beside V (Zvfh, Zvfhmin,
Zvfbfmin, Zvfbfwma, Zvbb, Zvkb, Zvbc, Zvkg, Zvkned, Zvknha, Zvknhb, Zvksed,
Zvksh) and the deeper page tables and Svadu (Sv48, Sv57, Svadu) with
-march, so that a core built without one can be held to a DoomV without it.
tests/lockstep/switches/ext_switches.S uses each once; this runs it on Sail
and DoomV in strict lock-step, first as Sail's configuration has them (all
on), then with each switched off -- in Sail's configuration and in DoomV's
-march alike, with what depends on it -- and then with all of them off.
Every run has to match Sail record for record: where an extension is off,
both have to trap on its instruction, or keep the CSR as it was. And each
switch has to matter: DoomV with everything on, against Sail with one
thing off, has to stop at a mismatch.

    python tools/verification/ext_switches.py
"""
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "verification"))
import lockstep_sail  # noqa: E402  (Sail's runner, its flags and config)

run_suite = lockstep_sail.run_suite
WORK = ROOT / "build" / "ext-switches"
SOURCE = ROOT / "tools" / "verification" / "tests" / "lockstep" / "switches" / "ext_switches.S"
TOOLCHAIN_MARCH = "rv64imafdcvh_zicsr_zvfh_zvbb_zvbc_zvkg_zvkned_zvknhb_zvksed_zvksh"

# Switched off together: the extension, and what the specification makes
# depend on it. Each is a list of (-march token, Sail configuration name).
VM48 = [("sv48", "Sv48")]
VM57 = [("sv57", "Sv57")]
CASES = {
    "zvfh": [("zvfh", "Zvfh")],
    "zvfhmin": [("zvfh", "Zvfh"), ("zvfhmin", "Zvfhmin")],
    "zvfbfwma": [("zvfbfwma", "Zvfbfwma")],
    "zvfbfmin": [("zvfbfwma", "Zvfbfwma"), ("zvfbfmin", "Zvfbfmin")],
    "zvbb": [("zvbb", "Zvbb")],
    "zvkb": [("zvbb", "Zvbb"), ("zvkb", "Zvkb")],
    "zvbc": [("zvbc", "Zvbc")],
    "zvkg": [("zvkg", "Zvkg")],
    "zvkned": [("zvkned", "Zvkned")],
    "zvknhb": [("zvknhb", "Zvknhb")],
    "zvknha": [("zvknhb", "Zvknhb"), ("zvknha", "Zvknha")],
    "zvksed": [("zvksed", "Zvksed")],
    "zvksh": [("zvksh", "Zvksh")],
    "sv57": VM57,
    "sv48": VM57 + VM48,
    "svadu": [("svadu", "Svadu")],
}


def sail_config(off_names: set, path: pathlib.Path):
    """Sail's configuration with these extensions not supported; a page-table
    mode off is off for satp, vsatp and hgatp alike."""
    cfg = json.loads(lockstep_sail.SAIL_CONFIG.read_text())
    for name in off_names:
        cfg["extensions"][name]["supported"] = False
    h = cfg["extensions"]["H"]
    for mode in ("Sv48", "Sv57"):
        if mode in off_names:
            h["vsatp_modes"][mode] = False
            h["hgatp_modes"][mode + "x4"] = False
    path.write_text(json.dumps(cfg, indent=2))


def one(name: str, off: list, elf: pathlib.Path, tohost: int):
    work = WORK / name
    work.mkdir(parents=True, exist_ok=True)
    config = work / "sail.json"
    sail_config({n for _, n in off}, config)
    trace = work / "sail.log"
    r = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", run_suite.WSL_SAIL, "--config",
                        lockstep_sail.wsl(config)] + lockstep_sail.SAIL_FLAGS
                       + ["--trace-output", lockstep_sail.wsl(trace), lockstep_sail.wsl(elf)],
                       capture_output=True, text=True, timeout=300, env=run_suite._env())
    said = (r.stdout or "") + (r.stderr or "")
    if "SUCCESS" not in said:
        return False, "Sail did not finish: " + said.strip()[-600:]
    def doomv(march):
        d = subprocess.run([str(ROOT / "riscv_doom.exe"), "-ng", str(ROOT / "tools/doom/doombuild/DOOM1.WAD"),
                            str(elf), "-march=" + march, f"-tohost={tohost:x}", f"-lockstep={trace}",
                            "-lockstep-strict", "-stopat=1000000"], cwd=work, capture_output=True, text=True,
                           timeout=300)
        return (d.stdout or "") + (d.stderr or "")

    tokens = [t for t in run_suite.SUITE_MARCH.split("_") if t not in {m for m, _ in off}]
    out = doomv("_".join(dict.fromkeys(tokens)))
    if "MISMATCH" in out:
        return False, out[out.index("lockstep: MISMATCH"):].strip()[:2000]
    # The switch matters: with it on, DoomV parts from this Sail.
    if off and "MISMATCH" not in doomv(run_suite.SUITE_MARCH):
        return False, "DoomV with everything on matches Sail with this off: the test does not see the switch"
    traps = trace.read_text(errors="replace").count("handling exc#illegal-instruction")
    verdict = next((l for l in out.splitlines() if l.startswith("lockstep: ")), "no verdict")
    ok = ("all matching" in verdict or "matched before the run ended" in verdict) and \
         (work / "tohost.log").exists() and (work / "tohost.log").read_text().strip() == "1"
    return ok, f"{verdict.removeprefix('lockstep: ')}; {traps} illegal-instruction traps"


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    elf = WORK / "ext_switches"
    r = subprocess.run(["wsl", "-d", "Ubuntu", "-u", "root", "--", "riscv64-unknown-elf-gcc",
                        "-march=" + TOOLCHAIN_MARCH, "-mabi=lp64d", "-nostdlib", "-static", "-Wl,-N",
                        "-Wl,-Ttext=0x80000000", "-Wl,--no-relax", "-o", lockstep_sail.wsl(elf),
                        lockstep_sail.wsl(SOURCE)], capture_output=True, text=True, env=run_suite._env())
    if r.returncode:
        print("cannot assemble the test: " + r.stderr)
        return 1
    tohost = run_suite.elf_symbols(elf)["tohost"]
    cases = {"all on": []}
    cases.update({f"{k} off": v for k, v in CASES.items()})
    cases["all off"] = list(dict.fromkeys(p for v in CASES.values() for p in v))
    failed = 0
    for name, off in cases.items():
        ok, detail = one(name.replace(" ", "-"), off, elf, tohost)
        failed += not ok
        print(f"{'pass' if ok else 'FAIL'} {name}: " + detail.replace("\n", "\n    "), flush=True)
    print("\nall pass" if not failed else f"\n{failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
