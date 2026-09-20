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
| **D**: data-access caches, inline register accessors, decode by reference, batched run loop | 46.9 | 60.0 | identical |
| **D + PGO** | 59.7 | 82.2 | identical |

That is 4.4x on the Linux boot and 3.6x on DOOM from the plain build, and 5.6x
and 4.9x with `pgo.py`. The baseline reads between 9.9 and 10.7 MIPS from run
to run, so each row was measured against its own reference run rather than
against a remembered number; the factors above use those pairs. The exact runs
are in [`RUNS.md`](RUNS.md), and the histograms beside them.

After D the interpreter spends its time on the instructions themselves: about
75-80% in `step_execute`, `fetch16`, `decode_and_dispatch` and `exec_32I`
together, with translation and PMP down to ~7% of a Linux boot and loads and
stores to ~7%.

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

### D: loads, stores, decode and the loop

* **Data-access caches.** `load_virtual` and `store_virtual`, for an access
  inside one page, keep the same kind of cache the fetch does -- one for loads
  and one for stores, since a page can be readable and not writable and a
  store is what sets D. A page is remembered after a successful access through
  the full path, only when it is RAM end to end with no second stage, no MPRV,
  and one PMP entry for the whole page, and it goes stale on the same events.
  A store keeps its side effects: the page holding `tohost` is never cached.
  A cached access reports itself exactly as the full path does -- into the
  access log lock-step compares against the reference, and for a store the
  bytes it wrote. The caches first switched themselves off whenever those logs
  were set, which was correct but left them the one part of the machine the
  Sail sweep never exercised; reporting instead of bypassing puts the fast
  paths under the instruction-by-instruction check too.
* **Register accessors inline.** `read_x`, `write_x`, `get_pc`, `set_pc`,
  `get_priv` and `read_csr` were defined in `registers.cpp`, so every
  instruction made several real calls; without LTO nothing could inline them.
* **Decode by reference.** `decode_and_dispatch` used to copy the decoded
  instruction out of its cache and then into its result, every step. It works
  from the cache entry and returns a pointer to it.
* **Batched run loop.** `cpu_loop` checked the input checkpoint and `-stopat`
  after every step. Every step that runs advances the count by exactly one, so
  the steps up to the next of those run as a plain loop and the checks happen
  once at its end, at the same counts. Lock-step keeps the step-by-step loop.

Besides the two workloads, an input script's run (DOOM, a right click in the
menu, 1,500M steps) and a 600M-step Linux boot were compared with the
previous build: identical. The input script's run was also done twice with the
same build -- identical again, which is the check for a dependence on host
timing or thread interleaving rather than on the old behaviour.

### Compiler and flags

The installed compiler is MinGW GCC 8.1 (2018), from the CodeBlocks bundle
`scripts/install_dependencies.ps1` relies on.

* **`-flto` is unusable with GCC 8.1.** Its LTO plugin warns *No symbol for
  section 'Extensions'* -- the C++17 inline variable in `extensions.hpp` -- and
  the linked emulator dies of heap corruption before printing anything. The
  same sources without it run correctly.
* **PGO works**, and is `pgo.py`. Two things it has to get right, both
  documented there: GCC 8.1's profiling runtime writes nothing at all when
  `-fprofile-dir` is a Git Bash path, so `GCOV_PREFIX` redirects it; and GCC 11
  and later name each profile after the *output* path, so the instrumented and
  final builds must be built to the same one.
* `-march=native` was not used: it would tie the binary to the build machine,
  and FMA contraction can change floating-point results between hosts.

**A newer GCC was measured, and is not the default.** GCC 14.2 (winlibs,
MSVCRT, POSIX threads) unpacked under `build/toolchains/`, every build with
`crash.log` identical to the baseline:

| build | linux MIPS | doom MIPS |
|---|---:|---:|
| GCC 8.1, plain (what `make` gives) | 49.2 | 60.5 |
| GCC 14.2, plain | 39.8 | 54.2 |
| GCC 14.2 + LTO | 43.7 | 53.6 |
| GCC 8.1 + PGO (`pgo.py`) | 59.7 | 82.2 |
| GCC 14.2 + PGO | 59.4 | 79.2 |
| GCC 14.2 + PGO + LTO | 64.8 | 86.0 |

Plain GCC 14 is 10-20% *slower* on this code than GCC 8, and LTO does not make
that back; with PGO the two are level. Only PGO and LTO together, which GCC 8
cannot produce, get ahead -- by 5-9%, for an 850 MB toolchain (a 245 MB
download) this machine does not otherwise have. So `make` and `pgo.py` keep the installed compiler, and
`pgo.py` takes the other route as an option:

```sh
TC=build/toolchains/gcc14/mingw64/bin
python performance/pgo.py --lto --cxx $TC/g++.exe --cc $TC/gcc.exe
```

GCC 14 did earn its keep once already: it rejected two headers that use
`uint32_t` without including `<cstdint>`, which GCC 8 accepted only because its
own headers happened to pull it in.

## What is left

The candidates the histograms point at now that fetch, loads and stores are
cached. Nothing here is measured except where it says so.

* **A newer compiler** has been measured rather than guessed -- see the table
  above. Plain GCC 14 is slower than GCC 8 here; only PGO and LTO together get
  ahead, by 5-9%, and that needs a toolchain this machine does not ship. What
  is still untried is a different compiler family: clang's optimizer makes
  rather different choices on interpreter dispatch, and neither it nor a
  UCRT-based GCC has been tried against the bundled SDL2.
* **Per-page pre-decode** was tried in its cheapest form -- give each cached
  code page its own table of decode slots, indexed by halfword offset, so that
  the page a step has already found for the fetch carries the decode too and
  the second lookup disappears. It measured 0.934x on doom with 16 tables, and
  0.955x with 64 and the clear-on-handover removed. It was dropped.

  The premise turned out to be wrong, and it is worth writing down because it
  was the reason this item was called the largest remaining gain. The decode
  cache is indexed `(pc >> 1) & CACHE_MASK`, so consecutive instructions
  already occupy consecutive slots: a per-page table is not a better layout
  than the shared array, it *is* that layout, over a 2048-slot window of it.
  Being megabytes wide costs nothing when only the hot window is ever touched.
  So there was no locality to win, and what remained was a branch and a pointer
  chase per step, which is what the numbers show.

  What that leaves of the idea is the eager form: decode a whole page up front
  and execute from it with no per-instruction tag check at all. The tag check
  is four comparisons on a line that is already hot, so the ceiling is low,
  while dropping it means invalidating the table exactly where the tag check
  invalidates itself today -- self-modifying code included. Low ceiling, high
  risk. The patch for the measured version is at
  `build/patches/page-decode-tables.patch`.
* **Dispatch through a handler pointer** in the decode cache entry was tried,
  measured and dropped: linux 0.998x and 0.992x, doom 0.922x, 0.982x and
  1.001x over five paired runs -- neutral at best, and the one low reading is
  what run-to-run noise looks like on doom. GCC already compiles the switch on
  Extension to a jump table, so there was no dispatch overhead to remove, and
  the indirect call through a pointer-to-member only takes inlining away. The
  patch is kept at `build/patches/handler-pointer.patch`, because a per-page
  pre-decode table would want exactly that handler resolved per entry.
* **The cost of a step itself.** `step_execute` is the largest single entry in
  the histogram, and it is now mostly bookkeeping: the trap-count snapshot, the
  committed/decoded flags, the history write, the clock and `minstret`. Two
  pieces of it have been taken (see the rows below): the interrupt check's four
  generation counters became one `EventGen`, and `record_history` masks instead
  of taking a signed modulo -- together 1.012-1.022x over four paired runs.
  What is left there is genuinely per-step state, not redundant lookups.

The lesson from the four changes measured in this pass: a win comes from
removing a redundant *lookup*, and only from that. Folding the two halfword
fetches into one page-cache lookup was worth 1.12-1.17x, because the second
fetch really was asking a question the first had already answered. Collapsing
four generation loads into one was worth 1.01-1.02x for the same reason, on a
smaller scale. Making dispatch arithmetic cheaper was worth nothing, and moving
the decode cache into the code page -- which looked like the same fusion, but
was not, since the two lookups answer different questions and the second was
already laid out well -- was worth less than nothing.

A JIT is deliberately not on this list. It could still be exact -- per-
instruction state, single-instruction blocks under lock-step -- but every
translated block would have to reproduce each instruction's effects and
interrupt timing, and that is a much larger surface to keep honest than the
caches above, each of which is a cache of an answer the interpreter still
computes the slow way whenever anything relevant changes.

## Checking a change

1. `python performance/bench.py linux --compare build/perf-baseline/riscv_doom.exe --label "..."`,
   and the same for `doom`: the hashes must match.
2. `python tools/verification/lockstep_sail.py`: every test still lock-steps
   against Sail, strictly.
3. `python scripts/verify.py`.
