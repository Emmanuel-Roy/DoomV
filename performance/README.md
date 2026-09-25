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
Under Clang it keeps the profile as `build/pgo/doomv.profdata`, and the
Makefile uses it from then on, so every `make` is the PGO build; training is
the only slow part, and it happens here rather than in `make`.
`scripts/build.py` runs it the first time there is a guest to train on.

```sh
python performance/pgo.py            # train, keep the profile, build riscv_doom.exe
python performance/pgo.py --bench    # ...and record it against the baseline
python performance/pgo.py --stale    # one line if the sources changed since training
make PROFILE=                        # a build without the profile
```

A profile goes stale as the code changes, and a stale one gives up a good part
of what PGO is worth, so `make` says when it links with one and
`scripts/build.py` retrains it -- see [What else a profile can be made to
do](#what-else-a-profile-can-be-made-to-do).

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

**Clang is much faster on this code, and PGO with ThinLTO is what makes it
so.** llvm-mingw 20260908 (Clang 23.1.1, MSVCRT, x86_64) unpacked under
`build/toolchains/`. The MSVCRT build rather than the UCRT one, so that the C
runtime matches the installed GCC and the bundled SDL2 and the only thing that
differs is the compiler. Every build below stops both workloads with a
`crash.log` identical to the baseline's:

| build | linux MIPS | doom MIPS |
|---|---:|---:|
| GCC 8.1, plain (what `make` gives without a Clang) | 55.3 | 69.5 |
| Clang 23, plain | 56.1 | 74.2 |
| Clang 23 + ThinLTO (what `make` gives) | 58.5 | 81.5 |
| GCC 8.1 + PGO (`pgo.py`) | 64.8 | 88.9 |
| Clang 23 + PGO | 64.4 | 90.3 |
| Clang 23 + PGO + ThinLTO | 80.0 | 113.2 |

Head to head on the same sources, Clang with PGO and ThinLTO is 1.284x on doom
and 1.254x on linux over the best the installed GCC can produce -- the two
workloads' figures move by a couple of points between runs, so read them as
1.25-1.31x rather than to three digits -- and it passes the whole gate in that
configuration: 373/373 strict lock-step against Sail, and
every suite in `scripts/verify.py`.

It did not pass at first, and what it caught was a bug in this project rather
than anything about Clang. Ten riscv-arch-test failures and three vector ones,
every single difference an `fflags` bit and never a computed value: NX was not
being raised by any `FCVT` or `VFNCVT`. The cause is that F/D are computed with
real host float arithmetic and the host's exception flags are read back, and
the twelve call sites doing that collected the flags *after* restoring the
rounding mode:

    clear -> fesetround(guest mode) -> compute -> fesetround(old) -> collect

C says `fesetround` establishes the rounding direction; nothing guarantees it
leaves the exception flags alone. The mingw-w64 runtime in the GCC 8.1 bundle
does a read-modify-write and preserves them, so that order worked for as long
as there was one toolchain in play. llvm-mingw's writes MXCSR whole and clears
the status bits with it, which zeroed the flag between raising it and reading
it. Collecting before restoring is correct under either runtime and is what the
code does now; `collect_fflags` in `ext_fp_common.hpp` says why. Both compilers
pass everything with the fix, and the GCC build's `crash.log` is unchanged.

Worth noting for its own sake: a second toolchain found a latent bug here that
373 lock-step tests and a 3,042-test vector suite could not, because on the
original runtime the code was not wrong in any observable way. That is a better
argument for keeping Clang buildable than the speed is.

The shape of the table is worth reading, because it is not the shape GCC 14's
was. Plain Clang is ahead of plain GCC 8 but only by 1.05-1.07x, and PGO alone
brings the two compilers level -- Clang's win is almost entirely in ThinLTO,
which is worth 1.02x on its own and 1.17-1.20x once there is a profile to go
with it. That combination is exactly the one GCC 8.1 cannot build at all: its
LTO plugin loses the `Extensions` inline variable and the binary dies of heap
corruption. So the gap is less "Clang optimizes interpreter dispatch better"
than "Clang can do the two things together, and they compound."

`make` and a bare `pgo.py` still use the installed GCC, since `make` should
work on a checkout with nothing unpacked. The Clang route is an option on
`pgo.py`, which learned Clang's profile flow to support it -- the instrumented
binary writes one `.profraw` per run rather than a `.gcda` per object, and
`llvm-profdata merge` has to fold them before `-fprofile-use` can read them:

```sh
TC=build/toolchains/llvm-mingw-20260908-msvcrt-x86_64/bin
python performance/pgo.py --lto --cxx $TC/clang++.exe --cc $TC/clang.exe
```

Clang built these sources without a single error or new warning on the first
attempt, which is some evidence the code is not leaning on GCC-specific
behaviour anywhere.

### What else a profile can be made to do

With PGO the default, the next question was whether the profile could be put to
better use. Everything below was measured on a Linux host with Clang 18, each
variant against the default profile *in the same session* (the host's absolute
speed drifted between sessions), in interleaved runs of 3-5, where noise is about
±3%. The default itself -- IR instrumentation, trained on doom and linux, with
ThinLTO -- measured 1.32-1.37x over ThinLTO alone on doom, 1.38-1.41x on linux,
1.36-1.37x on fbcon and 1.29-1.30x on a 500M-step doom `-input` session. fbcon
is the framebuffer-console workload from [RESEARCH.md](RESEARCH.md); it and the
`-input` session were never trained on.

| variant, against the default profile | doom | linux | fbcon | doom -input |
|---|---:|---:|---:|---:|
| doom trained to 300M steps, not 1000M | 0.99 | 1.00 | | |
| trained on doom only | 0.99 | 0.95 | | |
| trained on linux only | 0.98 | 0.97 | | |
| fbcon added to the training | 0.98 | 0.97 | 0.98 | 1.02 |
| frontend instrumentation (`-fprofile-instr-generate`) | 1.03-1.04 | 1.02-1.04 | 1.00 | 1.04 |
| ...the same, with the fast-loop patch applied | 0.89 | 0.99 | | |
| ExtTSP block layout (`-enable-ext-tsp-block-placement`) | 1.01 | 1.03 | 0.98 | 1.01 |
| hot/cold splitting (`-hot-cold-split`) | 1.00 | 1.02 | | |
| `-mllvm -pgso=false` | 1.01 | 0.98 | | |
| `-O2` instead of `-O3` | **0.96** | **0.96** | | |
| context-sensitive PGO (`-fcs-profile-generate`) | **0.85** | **0.84** | **0.85** | **0.87** |
| a profile three commits old | **0.87** | **0.85** | | |

Crash.log was the same as the plain build's for every one. What it says:

* **What is trained on hardly matters.** A doom-only profile runs linux within 5%
  of a linux-trained one and the other way round, and a workload no profile saw
  -- fbcon -- gains as much as the trained ones. The profile describes the
  interpreter's branches, not the guest's program. So training now runs doom to
  300M steps instead of 1000M: the same profile, in half the training time
  (19 s instead of 37 s here).
* **Staleness is what matters.** A profile trained three commits back -- before
  the framebuffer and decode-cache changes -- kept 1.18-1.20x over ThinLTO where
  a fresh one had 1.37-1.41x: it lost half of what PGO is worth. Clang matches a
  profile to each function by a hash of its control flow, and a function that
  changed builds as though it had none, silently, since the Makefile hides
  Clang's per-file warnings. So `pgo.py` now records a hash of every source file
  it trained on (`build/pgo/doomv.profdata.sources`), `pgo.py --stale` compares,
  the Makefile prints one line naming the changed files when it links with a
  stale profile, and `scripts/build.py` retrains. The check is by content, so
  touching a file, or coming back to the sources it was trained on, does not
  trip it; it takes about 40 ms.
* **Context-sensitive PGO is 15% slower**, on every workload. It is the one
  variant that is clearly worse, and not because it made the code bigger: the
  run loop, one function with everything inlined into it, comes out at 52.5 KB
  against 51.1 KB, and the binary as a whole is smaller. Where the time goes
  was not established; it is not worth pursuing at -15%.
* **Frontend instrumentation is not a consistent win.** It is 3-4% ahead on
  today's loop and 11% behind on doom with the fast-loop patch, so the default
  stays IR instrumentation, which is also what LLVM recommends.
* **Layout and splitting passes are noise.** ExtTSP layout and hot/cold
  splitting move nothing outside ±3%, and `-O2` is 4% slower.
* **With the fast-loop patch, PGO is worth much less**: 1.07x on doom and 1.17x on
  linux over ThinLTO, against 1.37-1.41x today, and a stale profile keeps about
  half of that. Most of what PGO does for today's loop is arrange its rarely-taken
  per-step checks, and the fast loop takes those checks out of the loop.

Two things were not measured. Training runs headless, so the window, dashboard
and display code never runs under it, and Clang treats code with no counts as
cold and optimizes it for size: `Gui::poll_input` is 1 KB in the PGO build and
3.7 KB without it, and `dashboard_loop` and `display_loop` shrink too. That
cannot slow the emulation, which is a thread of its own, and whether it slows
the window was not measured -- a windowed training run would settle it. And
the post-link and sampling tools, BOLT and AutoFDO, do not apply here: BOLT
rewrites ELF binaries only, not the Windows executable, and AutoFDO needs
hardware branch sampling this project has no way to collect on Windows.

### The workloads

`bench.py` measures three, each a different mix:

| workload | steps | MIPS | what it exercises |
|---|---|---|---|
| doom | 1000M | ~81 | the renderer, WAD reads, the framebuffer |
| linux | 300M | ~59 | OpenSBI, the kernel, BusyBox userspace |
| ubuntu | 3000M | ~54 | systemd, udev coldplug, a real userland off a virtio disk |

`ubuntu` is the newest and the heaviest, and it is heaviest for a reason worth
knowing: it is the only one doing sustained MMU work against a real
distribution rather than a single static binary. If a change helps the page
tables or the TLB, this is where it shows.

Two things about it are not obvious.

**It does not reach the desktop.** It boots on the XFCE device tree, and X is
twenty minutes of emulated time past where the measurement stops -- no
benchmark can wait for that. What it measures is the boot. Measuring the
running desktop would need a way to resume from an already-booted machine,
which this emulator has no facility for; `Snapshot` is the dashboard's view of
state, not a save file.

**Every run starts from a fresh copy of the image.** The root disk is opened
read-write and early boot writes to it -- the journal, the random seed -- so
benchmarking `ubuntu.img` directly would mean each run began from whatever the
last one left behind. Not reproducible, and not kind to an image that took
hours to build. `prepare_ubuntu()` copies it to `build/bench-ubuntu.img` first,
about two seconds for 4GB, outside the timed section. With that, two runs
produce an identical crash.log hash, which is what makes it usable as an
equivalence check the way the other two are.

It is marked `optional`, because it needs an `ubuntu.img` a fresh checkout does
not have. `pgo.py` skips it when training and `ram_backends.py` leaves it out
by default; naming it explicitly runs it.

```sh
python performance/bench.py ubuntu --compare build/perf-baseline/riscv_doom.exe --label "..."
```

### Does more RAM cost anything

No, not with the backend `make` builds. Throughput does not move with the size
of the guest, and startup barely does.

Throughput, doom, three runs each (doom rather than linux for a reason given
below):

| -ram= | median MIPS |
|---|---|
| 1G | 81.59 |
| 1500M | 82.01 |
| 2000M | 81.92 |

Flat, and the ranges overlap. Nothing in the hot path scales with the size of
the allocation: the bounds checks are one compare against `RAM_SPAN` whatever
it holds, and the decode and fetch caches are fixed-size and indexed by
address, not sized from RAM.

Startup is the one place the size is visible, and only for `vector`:

| backend | 1G | 8G | 16G |
|---|---:|---:|---:|
| pages (the default) | 0.03s | 0.05s | 0.39s |
| vector | 0.13s | 0.98s | 5.13s |

`vector` zeroes the whole allocation before the guest runs -- about 0.32s per
gigabyte -- and commits that much physical memory whether or not the guest ever
reads it. `pages` takes address space and lets the physical pages arrive on
first touch, so RAM the guest does not use costs nothing at all. That is a much
larger argument for the backend than the ~2% throughput it was chosen on, and
it is what makes `-ram=16G` a reasonable thing to type.

What does cost is the guest *touching* the memory, not having it. Past the
host's own RAM the host starts swapping, and no amount of emulator tuning
survives that. Size the guest to what it will use.

**Why doom and not linux for the table above.** Linux appears to get faster with
more memory -- 59.9 MIPS at 1G rising to 80.9 at 8G -- and it is not faster. The
workload runs a fixed 300M instructions, and a kernel with more memory spends
those instructions differently: more page clearing, which is cheap per
instruction, and proportionally less of everything else. The crash.log hashes
differ across the sizes, which is the tell -- it is a different program, not the
same one running faster. DOOM does the same work at any size, so it is the one
that can answer the question. (Its hashes differ too, because the WAD sits above
RAM and moves with it, but the instruction stream is the same work at a
different address.)

### How the guest's RAM is allocated

The guest's RAM is one flat allocation, and `GuestRam` backs it three ways so
they can be compared in one binary: `DOOMV_RAM_BACKEND=vector|pages|hugepages`.
`vector` is what it used to be, `pages` is VirtualAlloc or mmap, `hugepages`
asks those for large pages. `performance/ram_backends.py` runs the comparison:
five runs of each backend on each workload, in a clean directory per run, with
the crash.log hash checked every time.

| workload | vector | pages | hugepages |
|---|---|---|---|
| doom | 81.24 (81.01-81.64) | 81.70 (80.27-82.25) | 81.64 (80.04-81.85) |
| linux | 58.71 (58.51-58.88) | 59.83 (59.34-60.04) | 59.91 (58.47-60.09) |

Median MIPS, with the range across runs. Allocation cost is separate and is
measured on the buffer alone: 85.1 ms for `vector`, 0.0 ms for the other two,
because pages arrive zeroed on first touch rather than being written by the
allocator.

**`pages` wins on linux and is a wash on doom.** On linux the ranges do not
overlap at all -- vector tops out at 58.88 and pages starts at 59.34 -- so the
~1.9% is real. On doom the ranges overlap almost entirely and there is nothing
to claim. The difference between the two workloads is the interesting part:
Linux touches far more of the gigabyte, initialising its own structures and
filling a page cache, while DOOM's working set is tens of megabytes. The
backing matters in proportion to how much of the buffer is actually used.

**Large pages could not be had here at all**, which is why `hugepages` and
`pages` measure the same to within noise -- the first falls back to the second.
Windows gates them behind SeLockMemoryPrivilege ("Lock pages in memory"), which
an ordinary account does not hold, so `MEM_LARGE_PAGES` fails. `allocate()`
reports what it actually got through `active()` rather than what was asked for,
specifically so this shows up instead of looking like a measured win. Granting
that privilege needs an administrator and a re-login; on Linux `MADV_HUGEPAGE`
needs transparent hugepages enabled and is advisory even then.

So published figures showing large pages winning ~1.4x on a buffer this size
have not been reproduced here, and could not be: nothing in this table is
running on large pages. Those figures also measure random access across the
*whole* buffer, which is not either of these workloads. The honest expectation
is that they would matter more at `-ram=8G` and up, with a guest big enough to
use what it was given.

`hugepages` is the default, which means `pages` wherever large pages cannot be
had, and `pages` is the better of the two that are actually available.

A hash table for the decode cache was considered and not tried. It is already a
flat array indexed by `(pc >> 1) & CACHE_MASK`, which is O(1) with consecutive
instructions in consecutive slots; hashing would add work and an indirection to
replace a mask. See the per-page pre-decode entry below for what happened the
last time that array's layout looked improvable.

## What is left

[RESEARCH.md](RESEARCH.md) has the next round. Three changes from it have
landed -- both framebuffers through the data caches, a decode cache with room
for a kernel, and PGO as the default build -- and a fast-path loop, still a
patch, takes the result to 2.63x on doom, 2.46x on linux and 2.98x on a
workload that draws on the framebuffer console, with identical crash.logs. It
also has the measurements behind not splitting fetch/decode and execution
across two threads.

The candidates the histograms point at now that fetch, loads and stores are
cached. Nothing here is measured except where it says so.

* **Another compiler** has now been measured in both directions -- see the two
  tables above. GCC 14 is not worth it; Clang 23 with PGO and ThinLTO is worth
  1.31x on doom over the best the installed GCC can do, and is the fastest this
  emulator has been. What remains untried there is a UCRT-based toolchain
  against the bundled SDL2, which the MSVCRT llvm-mingw build was chosen
  specifically to avoid needing to answer.
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
