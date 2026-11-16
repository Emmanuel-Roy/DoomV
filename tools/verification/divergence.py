#!/usr/bin/env python3
"""Where Spike, Whisper and QEMU part from Sail -- one small program per case.

Each tests/divergence/*.S isolates one way a simulator differs from Sail
(found by co-running the riscv-arch-test suite, corun.py) and says in its
header why: a bug in that simulator, a choice the spec leaves open, or an
implementation-defined value. Its "#! <sim>: <what> <instruction>" lines are
the divergences it expects -- corun's field ("trap", "x10", "pc", ...) and
the start of the instruction's disassembly; every simulator it does not
name, DoomV included, must match Sail step for step. Anything else -- a
simulator that stops diverging, or diverges elsewhere -- is reported, so the
ledger stays true as the simulators are updated.

    python tools/verification/divergence.py                 # every case
    python tools/verification/divergence.py qemu_pmp_warl
    python tools/verification/divergence.py --multihart     # several harts
    python tools/verification/divergence.py --multihart --harts 2,4 --runs 20

--multihart runs the self-checking tests in tests/lockstep/multihart
(smp_selfcheck.S) on each simulator with several harts: sail_riscv_mh,
DoomV -harts, Spike -p, Whisper --harts, and QEMU -smp both with one host
thread per hart (MTTCG, the default) and with every hart on one thread.
Each reports the test's own verdict (tohost), and each is run --runs times:
the ones with a fixed schedule should give the same answer every time.

Every simulator is given the ISA riscv-arch-test's sail-RVA23S64 config
has -- the suites' march, which leaves out what DoomV implements without
being asked, plus Zca/Zcb/Zcd, Zicntr, Zihpm and Zicclsm.
"""
import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "tests" / "suites"))
import run_suite  # noqa: E402
import lockstep_sail as ls  # noqa: E402
import corun  # noqa: E402

CASES = HERE / "tests" / "divergence"
ELFS = ROOT / "build" / "divergence-elf"
MARCH = run_suite.SUITE_MARCH + corun.SAIL_IMPLIED
SIMS = ("doomv", "spike", "whisper", "qemu")
EXPECT = re.compile(r"^#!\s*(\w+):\s*(\S+)\s+(.*?)\s*$")


def expectations(source: pathlib.Path) -> dict:
    exp = {}
    for line in source.read_text().splitlines():
        m = EXPECT.match(line)
        if m:
            exp[m.group(1)] = (m.group(2), m.group(3))
    return exp


def check_cases(names, jobs) -> int:
    sources = sorted(CASES.glob("*.S"))
    if names:
        sources = [s for s in sources if any(n in s.stem for n in names)]
    ELFS.mkdir(parents=True, exist_ok=True)
    elfs = []
    for s in sources:
        elf = ELFS / (s.stem + ".elf")
        ls.compile_test(s, elf)
        elfs.append(elf)
    subprocess.run([sys.executable, str(HERE / "corun.py"), "--jobs", str(jobs), "--march", MARCH,
                    "--no-lockstep"] + [str(e) for e in elfs], capture_output=True, text=True)
    bad = 0
    for s, elf in zip(sources, elfs):
        exp = expectations(s)
        print(f"{s.stem}")
        report = corun.WORK / elf.name / "report.json"
        if not report.exists():
            print("   no report from corun")
            bad += 1
            continue
        sims = json.loads(report.read_text())["sims"]
        for sim in SIMS:
            r = sims.get(sim, {})
            st = r.get("status", "missing")
            if st == "match":
                got = "matches Sail"
            elif st == "diverged":
                got = f"{r['what']} at {r.get('instruction', '?')}: sail {r['sail']}, {sim} {r['theirs']}"
            else:
                got = f"{st}: {r.get('error', '')}"
            want = exp.get(sim)
            if want is None:
                ok = st == "match"
            else:
                ok = st == "diverged" and r["what"] == want[0] and r.get("instruction", "").startswith(want[1])
            bad += not ok
            mark = "ok  " if ok else "FAIL"
            print(f"   {mark} {sim:8} {got}" + ("" if ok else f"   (expected {'a match' if want is None else ' '.join(want)})"))
    print(f"\n{len(sources)} cases, {bad} not as recorded")
    return 1 if bad else 0


# ---- several harts -------------------------------------------------------------

def tohost_verdict(value):
    if value is None:
        return "no verdict"
    return "pass" if value == 1 else f"fail {value >> 1}"


def mh_sail(elf, n, timeout):
    code, out = corun.in_wsl([ls.WSL_SAIL_MH, "--harts", str(n), "--config", ls.wsl(ls.SAIL_CONFIG), ls.wsl(elf)],
                             timeout)
    if "SUCCESS" in out:
        return "pass"
    m = re.search(r"FAILURE: (\d+)", out)
    return f"fail {m.group(1)}" if m else "no verdict: " + (out.strip().splitlines() or ["?"])[-1][:80]


def mh_doomv(elf, n, timeout):
    work = ROOT / "build" / "divergence-mh" / f"{elf.stem}-doomv"
    work.mkdir(parents=True, exist_ok=True)
    log = work / "tohost.log"
    log.unlink(missing_ok=True)
    syms = run_suite.elf_symbols(elf)
    subprocess.run([str(corun.DOOMV)] + corun.elf_machine(elf, MARCH, 128, syms)
                   + [f"-harts={n}", "-stopat=50000000"], cwd=work, capture_output=True, timeout=timeout)
    if not log.exists():
        return "no verdict"
    v = log.read_text().split()
    return tohost_verdict(int(v[0], 0)) if v else "no verdict"


def mh_spike(elf, n, timeout):
    isa, _ = corun.spike_isa(MARCH, 128, elf, timeout)
    code, out = corun.in_wsl([corun.SPIKE, f"-p{n}", f"--isa={isa}", ls.wsl(elf)], timeout)
    if code is None:
        return "timed out"
    return "pass" if code == 0 else f"fail {code}"


def mh_whisper(elf, n, timeout):
    base, toks = corun.isa_tokens(MARCH)
    toks = [t for t in corun.smstateen_for_ssstateen(toks) if t not in ("sv48", "sv57", "sspm")]
    base += "".join(c for c in "sub" if c not in base[4:])
    code, out = corun.in_wsl([corun.WHISPER, "--harts", str(n), "--isa", "_".join([base] + toks),
                              "--target", ls.wsl(elf), "--tohostsym", "tohost", "--maxinst", "50000000"], timeout)
    if "Successful stop" in out:
        return "pass"
    m = re.search(r"Failed stop: .*?(\d+)\s*$", out, re.M)
    if "Failed stop" in out:
        return "fail" + (f" {int(m.group(1)) >> 1}" if m else "")
    return "no verdict: " + (out.strip().splitlines() or ["?"])[-1][:80]


def mh_qemu(elf, n, timeout, threads):
    out_dir = ROOT / "build" / "divergence-mh"
    out_dir.mkdir(parents=True, exist_ok=True)
    cpu, _ = corun.qemu_cpu(MARCH, 128, elf, timeout)
    img = corun.sized_htif(elf, out_dir)
    code, out = corun.in_wsl(["timeout", str(timeout), corun.QEMU, "-machine", "spike", "-smp", str(n),
                              "-cpu", cpu, "-bios", "none", "-kernel", ls.wsl(img), "-nographic",
                              "-monitor", "none", "-serial", "none", "-accel", f"tcg,thread={threads}"],
                             timeout + 30)
    if code == 124 or code is None:
        return "timed out"
    return "pass" if code == 0 else f"fail {code}"


def check_multihart(counts, runs, timeout) -> int:
    sources = sorted((HERE / "tests" / "lockstep" / "multihart").glob("smp_selfcheck*.S"))
    ELFS.mkdir(parents=True, exist_ok=True)
    runners = [("sail", lambda e, n: mh_sail(e, n, timeout)),
               ("doomv", lambda e, n: mh_doomv(e, n, timeout)),
               ("spike", lambda e, n: mh_spike(e, n, timeout)),
               ("whisper", lambda e, n: mh_whisper(e, n, timeout)),
               ("qemu mttcg", lambda e, n: mh_qemu(e, n, timeout, "multi")),
               ("qemu 1-thread", lambda e, n: mh_qemu(e, n, timeout, "single"))]
    for s in sources:
        for n in counts:
            elf = ELFS / f"{s.stem}-h{n}.elf"
            ls.compile_test(s, elf, [f"-DNHARTS={n}"])
            print(f"{s.stem}, {n} harts, {runs} runs each")
            for name, run in runners:
                seen = collections.Counter(run(elf, n) for _ in range(runs))
                print(f"   {name:14} " + ", ".join(f"{v} x{c}" for v, c in seen.most_common()))
    print("\n(the test's codes: fail 1 = AMO count wrong, 2 = LR/SC count wrong, 3 = the ABA SC succeeded)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="only the cases whose name contains one of these")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--multihart", action="store_true", help="the multi-hart self-checks instead")
    ap.add_argument("--harts", default="2,4")
    ap.add_argument("--runs", type=int, default=5, help="with --multihart: runs of each simulator")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()
    if args.multihart:
        return check_multihart([int(n) for n in args.harts.split(",")], args.runs, args.timeout)
    return check_cases(args.names, args.jobs)


if __name__ == "__main__":
    sys.exit(main())
