# Performance

How fast DoomV runs, where its time goes, and what has been done about it.
Every change here had to keep the machine executing the same instructions with
the same results -- lock-step against Sail compares instruction by instruction,
so a speed-up is only kept if it changes speed and nothing else.

## Tools

**`bench.py`** runs a workload headless to an exact step count and records the
run under `runs/`, with a line in [`RUNS.md`](RUNS.md):

```sh
python performance/bench.py linux                       # current riscv_doom.exe
python performance/bench.py doom --profile --label "why"   # with a histogram
python performance/bench.py linux --compare build/perf-baseline/riscv_doom.exe --label "change"
```

| workload | what runs | steps |
|---|---|---:|
| `linux` | OpenSBI, the kernel and BusyBox from the initramfs, to the shell | 300M |
| `doom`  | DOOM's startup and attract-mode demo | 1,000M |

Each run's `run.json` holds the commit, the emulator's hash, the time, MIPS,
and the sha256 of the `crash.log` the run stopped with. That file is the whole
machine state plus the last 4096 instructions, so **two emulators that stop
with the same hash ran the workload identically**. `--compare` runs a second
emulator (usually the baseline) on the same workload and exits non-zero if the
hashes differ.

**`sampler.py`** is the profiler `--profile` uses: a histogram of where the
host's time goes. Every 2 ms it suspends the emulator's CPU thread, reads the
instruction pointer from its context, and resumes it; addresses are mapped to
functions with the executable's own symbol table (`nm`), after correcting for
where Windows loaded it. Inlined functions count toward the function they were
inlined into. It needs nothing installed and no special build.

**`pgo.py`** builds `riscv_doom.exe` with profile-guided optimization: an
instrumented build, training on the two workloads, then the optimized build.
`make` stays the plain build, because this one runs the emulator in the middle
and takes a few minutes.

```sh
python performance/pgo.py            # riscv_doom.exe, PGO
python performance/pgo.py --bench    # ...and record it against the baseline
```

`build/perf-baseline/` holds the emulator from before this work (`eaa9628`),
which every comparison below ran against. `build/` is not committed, so to
compare against it on another checkout, build that commit on its own and copy
the emulator in beside its DLL:

```sh
git worktree add ../doomv-baseline eaa9628
make -C ../doomv-baseline -j4
mkdir -p build/perf-baseline
cp ../doomv-baseline/riscv_doom.exe SDL2.dll build/perf-baseline/
```

## What was done

Measured on the same machine, one run at a time. The baseline, from its
profile:

| linux 300M, baseline | share |
|---|---:|
| `Imsic::topei_value` | 25.7% |
| `pmp::check` | 11.7% |
| `translate_or_trap` | 9.4% |
| `mmu_translate` | 8.5% |
| `step_execute` | 7.6% |
| `exec_32I` | 7.2% |
| `compute_mip` | 5.2% |

A quarter of a Linux boot was spent rescanning the interrupt files for a
pending interrupt before every instruction, and another third translating and
permission-checking the instruction fetch.

| step | linux MIPS | doom MIPS | crash.log vs baseline |
|---|---:|---:|---|
| baseline `eaa9628` | 10.5 | 16.6 | -- |
| **A**: interrupt check cache, IMSIC `topei` kept current, lighter history, counter enables cached, no breakpoint check without breakpoints | 17.9 | 20.6 | identical |
| **B**: fetch cache, PMP over enabled entries only, RAM-first `write32` | 27.2 | 34.5 | identical |
| **C**: the cache checks inline | 29.6 | 37.9 | identical |
| **C + PGO** | 36.3 | 50.3 | identical |

That is 3.4x on the Linux boot and 3.0x on DOOM. `pgo.py --bench`, rerun from
scratch, measured 34.8 and 50.2 against a baseline that ran at 9.9 and 16.4
that time -- the same factors. The exact runs are in [`RUNS.md`](RUNS.md), and
the histograms beside them.

### A: the interrupt check

`check_and_take_interrupt` ran before every instruction, and rebuilt `mip` from
the timer, both IMSIC files and a dozen CSRs to do it. Its answer only changes
when one of those does, so it is skipped while none has:

* Every CSR write and every change of privilege or V bumps
  `Registers::state_gen`; the IMSIC files, `mtimecmp` writes and `misa` changes
  each have a generation too.
* `mtime` only grows, so a check that found nothing stays valid until it reaches
  the next compare value above it (`mtimecmp`, `stimecmp`, `vstimecmp`; with a
  nonzero `htimedelta`, every tick).

An interrupt is therefore taken at exactly the step it was before. The IMSIC's
top pending identity is recomputed when the file changes instead of on every
read. The instruction history, written every step, keeps only the pc and
encoding (all `crash.log` prints); the dashboard decodes the entries it shows.

### B: the fetch

A fetch was two translations, two PMP checks and two memory reads per
four-byte instruction. `DoomSystem::fetch16` caches a code page once a fetch
from it has succeeded through that full path, and only a page every fetch
inside would get the same answer for: RAM end to end, no second translation
stage, and one PMP entry deciding the whole page (`pmp::page_permits`). It
caches the translation and permission, never the bytes -- those are read from
RAM each time, so self-modifying code is fetched as written. Any CSR write,
privilege change, TLB flush or extension change drops it. PMP checks loop over
the enabled entries only, and `write32` tests RAM before the device windows.

### Compiler flags

* **`-flto` was rejected.** With this toolchain (MinGW GCC 8.1) the LTO plugin
  warns *No symbol for section 'Extensions'* -- the C++17 inline variable in
  `extensions.hpp` -- and the linked emulator dies of heap corruption before it
  prints anything. The same sources without it run correctly. It may be worth
  trying again with a newer GCC.
* **PGO works**, and is `pgo.py`. It needed two workarounds, documented there:
  the profiling runtime cannot write to Git Bash paths and fails silently, so
  `GCOV_PREFIX` redirects it; and `-fprofile-use` needs the Windows path too.
* `-march=native` was not used: it would tie the binary to the build machine,
  and FMA contraction can change floating-point results between hosts.

## Checking a change

1. `python performance/bench.py linux --compare build/perf-baseline/riscv_doom.exe --label "..."`,
   and the same for `doom`: the hashes must match.
2. `python tools/verification/lockstep_sail.py`: every test still lock-steps
   against Sail, strictly.
3. `python scripts/verify.py`.
