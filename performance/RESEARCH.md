# Faster, and still deterministic

Research into how much faster DoomV can go without giving up the property the
whole verification story rests on: the same guest with the same inputs runs
the same instructions to the same state, every time.

The short answer is **1.9-2.6x, measured, with byte-identical results**, from a
prototype in [`patches/fast-loop.patch`](patches/fast-loop.patch), and more
after that from well-understood techniques that keep the same guarantee. The
prototype is not a finished change -- it has not been through the gate -- but
every number below comes from a run whose machine state was compared with the
unmodified emulator's and found identical.

| build (Clang, PGO + ThinLTO on both sides) | doom 1000M | linux 300M |
|---|---:|---:|
| unmodified `8387f26` | 39.7 MIPS | 27.6 MIPS |
| with `patches/fast-loop.patch` | 101.9 MIPS | 53.2 MIPS |
| **speed-up** | **2.57x** | **1.93x** |
| crash.log | identical | identical |

Three of the follow-ups below have since landed on their own -- the
framebuffers through the data caches, a decode cache sized for a kernel, and
PGO as the default build (see [What has landed](#what-has-landed)) -- and the
patch has been refreshed on top of them. Against the same unmodified build:

| build (Clang, PGO + ThinLTO) | doom 1000M | linux 300M | fbcon 1500M |
|---|---:|---:|---:|
| unmodified `8387f26` | 40.1 MIPS | 28.0 MIPS | 30.5 MIPS |
| `3771294`, the three landed changes | 40.5 (1.01x) | 28.5 (1.02x) | 33.6 (1.10x) |
| `3771294` + `patches/fast-loop.patch` | **105.4 (2.63x)** | **69.0 (2.46x)** | **91.0 (2.98x)** |
| crash.log | identical | identical | identical |

`fbcon` is described under [What has landed](#what-has-landed). The fast loop is
worth more on Linux now than it was (2.46x against 1.93x) because the landed
changes remove misses it used to fall back on.

<a id="what-has-landed"></a>
## What has landed

Three commits, each measured against the one before it on the same Linux host,
in interleaved runs, and each checked to leave crash.log -- and guest RAM, both
framebuffers, their counters, `minstret`, `mcycle`, `mtime` and the MMIO tick
counter -- identical to the unmodified emulator's.

**Framebuffer loads and stores through the data caches** (`c3015b9`). Change 3
of the prototype, for both framebuffers. The data caches admit a whole page of
either, and a cached store bumps the host-side counters the uncached path
would -- DOOM's by the store's size, Linux's once per `write8` and once per
`write32` -- so even those come out the same. 1.05x on doom, 1.03x on linux
under PGO, and 1.16x (plain) on a workload that draws: the BusyBox boot on a
kernel built with the framebuffer console, then `dmesg > /dev/tty0` six times
from a stdin file, 123M framebuffer stores in 1.5G steps. The linux figures
here were measured with the kernel `Image` committed under `tools/linux/linux/`,
which predates `FB_SIMPLE` and never draws; the one `scripts/build.py` builds,
which `bench.py` boots, draws its boot log, so its linux workload should show
some of this too. The Ubuntu desktops draw through the same aperture, and were
not measured.

**The decode cache** (`9af6be5`). Change 5 of the prototype, a read of `misa`
no longer emptying the cache, plus 32-byte entries (the mnemonic, a third of
every entry, only ever came from `Decoder::describe`) and 2^19 of them. A
BusyBox boot's decode misses in 300M steps went from 8.14M to 0.65M. On today's
step loop that is worth nothing measurable -- linux 1.00x under PGO, doom 0.99x
plain and 0.95x under PGO, the last with fewer instructions per step and more
simulated mispredictions, which points at code layout -- because at ~220 host
instructions a step the misses were a couple of per cent. Under the fast loop,
where a step is under 90, it is the main difference between the patch's 1.93x
on linux before and 2.46x now. It is in its own commit so that it can be
judged on its own.

**PGO by default** (`3771294`). Under Clang, `pgo.py` keeps its profile as
`build/pgo/doomv.profdata`, every `make` builds with it, and
`scripts/build.py` trains one the first time it has a guest to train on, so
the fastest build is the one that runs and the one the gate tests. 1.44x over
ThinLTO alone on doom here.

<a id="threads"></a>
## A fetch/decode thread feeding an execute thread

The idea: one thread fetches and decodes instructions and hands them over,
another executes them. It was measured rather than built, because two numbers
decide it.

**What decoding costs now.** A handoff can take work off the executing thread
only if that work does not depend on execution. Fetch does -- the next pc is a
result of the instruction before it, and so are traps, interrupts and every CSR
write that changes translation -- so a fetch thread would have to predict the
path, and the executing thread would still have to check each instruction it
is handed against its own pc and page state. That check is exactly what the
fetch and decode caches do today. What moves is the decode itself, on a miss,
and after the decode cache commit a linux boot spends **0.12% of its time**
decoding (`decode_i`, `decode_compressed`, `classify` and the rest, by `perf`).
That is the most a decode thread could save.

**What a handoff costs.** A single-producer, single-consumer ring of 32-byte
records, on this 4-core host:

| | per record |
|---|---:|
| written and read on one thread | 1.5 ns |
| handed over, index published per record | 3.1 ns |
| handed over, published every 8 / 64 / 512 | 1.9 / 1.9 / 1.8 ns |
| a round trip, one thread waiting for the other | **593 ns** |

Streaming is cheap. The round trip is the problem: every time execution goes
somewhere the fetch thread did not predict -- a mispredicted branch, a trap,
an interrupt, a write to `satp` -- the fetch thread has to be told and start
again, and the executing thread waits for it. This host is virtualized; on
bare metal a round trip is typically nearer 100 ns. Either way it is many
steps' worth, at 10-35 ns a step. A 300M-step linux boot spends under 20 ms
decoding in total, and runs 32.7M conditional branches, 29.9M taken branches
and jumps and 12.7K traps and interrupts. A fetch thread that guessed 99% of
those branches right would still be redirected 330K times: 0.19 s at this
host's round trip, 33 ms at 100 ns, either way more than the decoding
it would take away.

**Determinism** would survive it, with care: a decode is a pure function of
the bytes and the extension set, so which thread computed it cannot matter as
long as entries are published atomically and still checked by tag. The cost is
the reason not to, not the risk.

**What does work** is the same idea kept on one thread: decode ahead of
execution, then execute without per-instruction checks. That is the fast loop,
and the next step after it, pre-decoded blocks (below). They split the work the
way the question suggests -- decoding once, executing many times -- without a
thread boundary in the middle of every branch.

## What determinism asks of an optimization

Every speed-up so far in [README.md](README.md) was a cache of an answer the
interpreter still computes the slow way when anything relevant changes. That is
the rule this research kept to, stated as an invariant:

> A shortcut is allowed to skip work only when the work provably cannot come
> out differently -- and "provably" means it depends only on guest state that
> the shortcut itself cannot have changed.

Three corollaries fall out of it:

* **Anything keyed to the host is out.** Host time, thread interleaving, the
  order a host thread happens to finish in, or a batch size that depends on how
  fast the host is going. The machine already refuses all of these (input is
  committed on instruction counts, devices complete on the CPU thread).
* **Anything keyed to guest state is in, if the key is complete.** A cache is
  exact exactly when its key covers everything the cached answer depends on.
  Most of what follows is either a new cache, or a key that was *more*
  conservative than it needed to be.
* **Bookkeeping may be deferred if nothing can observe it in between.** A
  counter the guest can only read through a CSR instruction or a device register
  can be added up and applied in one go, provided every path that could read
  it first brings it up to date.

## How this was measured

On Linux rather than the Windows machine the rest of this directory was
measured on, so absolute MIPS here are lower and not comparable with
[RUNS.md](RUNS.md); ratios between two builds on the same machine are what
count.

* Host: 4 vCPUs of an Intel Xeon (Cascade Lake) at 2.8 GHz under KVM, Ubuntu
  24.04. No hardware performance counters were exposed to the guest, so `perf`
  sampled on the CPU clock and branch/cache behaviour comes from callgrind's
  simulation.
* Emulator: Clang 18 (as `make` prefers), built from the `Makefile`'s own
  source list with `-O3`, and for the best-vs-best rows with ThinLTO and a
  Clang instrumentation profile trained on both workloads, as `pgo.py` does.
* Guests: DOOM rebuilt from `tools/doom/doombuild` with the xPack
  `riscv-none-elf-gcc` 14.2.0-3 the Makefile names; the committed kernel
  `Image`, `doomv.dtb` and `initramfs.cpio`; OpenSBI built by
  `tools/linux/opensbi/build.sh`. Because the DOOM ELF was rebuilt, its
  crash.log hashes (`182e8970854e` at 1000M) differ from `RUNS.md`'s; they are
  consistent across every build here.
* Workloads: `bench.py`'s two, headless to an exact `-stopat` -- DOOM's attract
  demo and the BusyBox boot, which reaches the shell by 300M steps.

**The equivalence check** is stronger than crash.log alone, because crash.log
does not record everything the prototype touches. The prototype takes a
`DOOMV_FAST=0|1` switch, so the old loop and the new one run in one binary, and
with `DOOMV_STATEDUMP=1` a stopped run also writes `minstret`, `mcycle`, `mtime`,
the tick phase, DOOM's `MMIO_TICK` counter, pc, a hash of all of guest RAM, a
hash of both framebuffers and the host-side framebuffer write counters. Both
files matched between the two loops, and crash.log matched the *unmodified*
emulator, at:

* DOOM: 1M, 12,345,677, 55,555,555, 99,999,999, 123,456,789, 300M and 1000M
  steps -- odd counts deliberately, so a run stops mid-tick and mid-batch;
* Linux: 3,333,333, 77,777,777, 120,000,001 and 300M steps;
* DOOM driven by an `-input` script (menu, new game, walking, firing, a mouse
  move and click) for 500M steps: the old loop, the new loop and the
  unmodified emulator all stopped on the same crash.log.

## Where the time goes

callgrind, counting host instructions executed per guest step, first 8M steps
of DOOM:

| build | host instructions per guest instruction |
|---|---:|
| unmodified, `-O3` | 283 |
| unmodified, PGO + ThinLTO | 218 |
| prototype, `-O3` | 88 |
| prototype, PGO + ThinLTO | 87 |

In the unmodified plain build the 283 split as `step_execute` 104,
`decode_and_dispatch` 63, `exec_32I` 42, `cpu_loop` 22, and the rest in loads,
stores and `step_instructions`. The instruction's own work -- the add, the
compare, the load -- is a handful of those. The rest is:

* **Per-step checks whose answer almost never changes.** The halt flag, the
  counter-enable key, the interrupt key and deadline, `debugger.may_halt()`
  twice, `Extensions.C`, the tohost word, `wait_request`, Zicfilp's `elp`, the
  vector and FP unit enables -- each two to seven instructions, every step.
* **Three real calls per step** (`step_execute`, `decode_and_dispatch`,
  `exec_32I`), each saving and restoring callee-saved registers, and forcing
  `pc`, the counters and the history index back through memory. PGO and LTO
  inline all three into `cpu_loop` -- 283 host instructions a step become 218 --
  which is a large part of why they are worth 1.44x here.
* **Per-step bookkeeping** that nothing reads between steps: `instr_count`,
  `ms_accum` and its tick loop, `tick_phase`, `mtime`, `mcycle`, `minstret`,
  `step_committed` and friends.
* **The history ring.** 4096 entries of 16 bytes is 64 KB, twice a 32 KB L1.
  callgrind puts 94% of all L1 write misses in `step_execute`, one every four
  steps -- a new line of the ring each time.

What it is *not* is dispatch. `Rohou, Swamy and Seznec` showed in 2015 that
modern indirect predictors make the switch-dispatch branch far less important
than interpreter folklore says, which is consistent with the handler-pointer
experiment in README.md measuring as noise. (callgrind's predictor, a
last-target BTB, reports 74% of `exec_32I`'s opcode-switch jumps mispredicted
and 39% of all indirect jumps; that model is much weaker than a real TAGE-style
predictor and should not be read as the hardware number.)

## The prototype

(3) and (5) have since landed on their own, and the refreshed patch carries
(1), (2) and (4); the numbers in this section are from the first version.

`patches/fast-loop.patch`, five changes. The first is the structural one and
(2) is part of it; (3) to (5) are cache changes that help the old loop too, but
matter most once the loop itself is cheap. The table adds them in the order they
were found.

| step (plain `-O3`, same binary, old loop vs new) | doom 300M | linux 300M |
|---|---:|---:|
| old loop | 27.4 | 21.7 |
| 1. fast-path loop | 77-80 | 39.0 |
| 2. + page-straddling fetch | 79.4 | -- |
| 3. + framebuffer stores cached | **100.4** | -- |
| 4. + `mstatus` writes invalidate only what they affect | -- | 48.8 |
| 5. + a `misa` read no longer flushes the decode cache | -- | **52.6** |

The old-loop figures moved between 26.2 and 28.4 (DOOM) and 18.4 and 22.7
(Linux) across binaries and time of day on this shared machine, so the rows are
each against the old loop in the same binary at about the same time, not
against one remembered number.

### 1. A fast-path loop with the checks hoisted out

`DoomSystem::run_fast(n)` replaces `for (k < n) step()` in `cpu_loop` and does
exactly what those `n` calls do.

It rests on one observation about a *simple* step: an I, C or M instruction
that is not a CSR or system instruction, fetched from a page already in the
fetch cache, whose load or store (if it has one) hits the data caches. Such a
step can change the x registers, pc and plain RAM -- and nothing else. It
cannot write a CSR, change privilege or V, flush the TLB, touch a device or
change the extensions. So it cannot move `state_gen`, `EventGen`, the TLB
generation or `ExtensionsEpoch`, and therefore cannot change the answer to any
per-step check: the fetch and data cache keys, the counter enables, the
interrupt decision.

So `run_fast` makes the checks once at the start of a run, and then runs simple
steps with only what each one actually needs -- a page compare for the fetch, the
decode cache's tag, a single flat `switch` on an operation id computed when the
decode entry was filled, and the history write. Anything that is not simple --
a cache miss, a device, a CSR, anything that might trap -- ends the run *before
that step has changed anything*, and the step is taken by `step()`, which stays
the definition of what a step is. Then a new run starts.

Two things need care, and both are exact rather than approximate:

* **The one input to the interrupt check that simple steps do change is
  `mtime`**, which they advance every second step. `interrupt_may_be_due`
  already reduces the timer to a deadline; step *j* of a run sees
  `mtime + (tick_phase + j) / 2`, so the run is capped at
  `2 * (deadline - mtime) - tick_phase` steps, and the step that reaches the
  deadline goes through `step()` and its full interrupt check -- at the same
  count it always did.
* **What simple steps do to state nothing can read mid-run** -- the step count,
  `ms_accum`/`MMIO_TICK`, `tick_phase`, `mtime`, `mcycle`, `minstret`, the
  `step_*` flags -- is summed and applied once when the run ends, before
  anything else can look. Everything that could read those values (a CSR
  instruction, a device load, `service_input`, `-stopat`, the snapshot) is on
  the other side of a run boundary.

The run is also bounded by the `n` that `cpu_loop` already computes from the
input period, `-stopat` and the burst budget, so every checkpoint lands on the
same count. Tracing, lock-step, breakpoints, a watched `tohost`, the access and
store logs, C or RV64 turned off and Zicfilp all send every step to `step()`, so
none of those paths changed at all.

Coverage at the end: DOOM runs 99.97% of its steps in the fast loop, the Linux
boot 97.6%.

### 2-3. DOOM's pixel loop

The fast-path counters pointed straight at one loop. DOOM's `cmap_to_fb`, which
converts every pixel of every frame, ends in a `bne` at `0x8002dffe` -- a
four-byte instruction straddling a page boundary, which the one-lookup fetch
cannot take -- and it stores every pixel to `MMIO_FB`. Both sent every iteration
to the slow path, 5.4M times each in 300M steps.

* The straddling fetch is two halfword fetches from two cached pages, which is
  exactly what `fetch16` does twice.
* The framebuffer store was the expensive one: a store-cache miss, a full
  translation and PMP check, then `write32` testing every device window before
  falling through to four `write8`s, each with a `StoreCapture` and an atomic
  generation bump. DOOM's framebuffer is plain bytes with no read side effects,
  and a store's only other effects are `fb_write_count` and `fb_gen`, both
  host-side (the FPS figure and the display thread's "frame finished" test) and
  both bumped by exactly the store's size. So the store cache now admits whole
  framebuffer pages, and a hit bumps the two counters by the size, as the byte
  path did. The guest cannot tell; even the host-side counters are identical.

That is DOOM from ~80 to ~100 MIPS in the plain build.

The Linux framebuffer (`LFB_BASE`) goes down the same byte path and could get
the same treatment -- its counter rule differs (`lfb_gen` counts words for
`write32` and bytes for `write8`), which is why it was left out of the
prototype. For the Ubuntu desktops, which draw through that aperture, it is
likely the biggest single win left; it was not measured here.

### 4. `mstatus` writes invalidated every translation cache

In 300M steps of the Linux boot there are 1.39M CSR writes and **1.24M of them
are `mstatus`** -- the kernel toggling `sstatus.SIE` around critical sections.
`write_csr` bumps `state_gen` on every write, and `state_gen` is part of the
fetch and data cache keys, so every `local_irq_save` threw every cached
translation away. SIE has nothing to do with translation.

The prototype bumps `state_gen` for an `mstatus` write only when a bit that a
`state_gen`-keyed decision reads has changed: MPRV, MPP, SUM, MXR, TVM, MPV,
GVA and the three endianness bits -- a deliberately generous set. `EventGen`
still moves on every write, since the interrupt enables are what it is for.
Fetch misses fell from 4.0M to 0.44M, load misses from 5.4M to 0.24M, store
misses from 2.4M to 0.14M.

This is the change that needs the most review: the mask has to contain every
bit any cache keyed on `state_gen` depends on. The strict Sail lock-step sweep
is the right check for it, and it could not be run here.

### 5. Reading `misa` flushed the decode cache

After (4), 5.7M of the remaining 7.8M decode-cache misses were *epoch*
mismatches. The CSR path calls `write_misa` for CSRRS/CSRRC with nothing to set
-- a plain `csrr misa` -- and `write_misa` bumped `ExtensionsEpoch`
unconditionally, discarding every decode. OpenSBI reads `misa` on its trap path,
so every timer interrupt through M-mode emptied the decode cache.

That is worth a second look for its own sake. Zicsr says a CSRRS or CSRRC with
`rs1=x0` does not write the CSR at all and has none of a write's side effects,
and here a read of `misa` runs the whole write path, TLB flush included.

A decode depends only on the instruction's bytes and the extension set, so the
prototype bumps the epoch only when the set actually changed. The TLB flush in
the same function was left alone on purpose: unlike the decode cache, a TLB is
guest-visible to a guest that edits a PTE without `sfence.vma`, so whether a
read of `misa` should flush it is a question about matching Sail, not about
speed. It is 23K flushes in 300M steps and not worth the risk.

On the unmodified step loop, the same binary's cache changes alone -- (3) to
(5) and the larger caches -- are worth 1.08x on Linux (20.0 to 21.7 MIPS); they
matter much more once the loop itself is cheap.

### What the prototype does not do

It was measured, not gated. `scripts/ci.py` -- the build, strict lock-step
against Sail and every suite in `verify.py` -- needs the Sail model and the
cross-built suites, which were not available here. DOOM and the BusyBox boot
exercise M, S and U mode, timer interrupts, paging and traps, but not the H
extension, V, F/D in anger, or Ubuntu. By construction those fall back to
`step()` (a hypervisor guest's pages are never cached, V and F/D are never
simple), but "by construction" is what the gate is for.

Also left in the patch, because it is a research artifact: the `DOOMV_FAST`,
`DOOMV_STATEDUMP`, `DOOMV_FASTSTATS` and `DOOMV_COARSE` switches, per-CSR write
counters, and fetch and data caches grown from 256 to 4096 entries (worth a few
per cent on Linux at most; the invalidation fixes were what mattered).

## What to do next, in order

Ranked by expected gain for the risk. Each keeps the invariant above; the
column says why.

| # | change | expected | why it stays deterministic |
|---|---|---|---|
| 1 | Adopt the fast-path loop, (1), (2) and (4) above | 2.4-3.0x, measured | every skipped check is one simple steps cannot change; everything else runs through `step()` |
| 2 | ~~The same store-cache treatment for the Linux framebuffer~~ landed, `c3015b9` | 1.16x on fbcon, measured | only host-side counters are involved, bumped exactly as the byte path does |
| 3 | Pre-decoded blocks with per-block counting | est. 1.5-2x more | QEMU's icount model: counts are exact because the budget is checked before a block runs and a block that will not fit runs a step at a time |
| 4 | ~~A decode cache big enough for a kernel, with smaller entries~~ landed, `9af6be5` | none on today's loop; part of the fast loop's 2.46x on linux | it is a cache tagged on pc and bytes; any size or layout is exact |
| 5 | ~~PGO + ThinLTO as the build people actually run~~ landed, `3771294` | 1.44x, measured | same sources; crash.log identical, as README.md already shows |
| 6 | Snapshots, to resume a booted machine | minutes to seconds for anything past boot | a snapshot is the whole machine state; restored and run on, it must match a straight run |
| 7 | Superinstructions, `musttail` dispatch, BOLT | small, each needs measuring | pure host-code changes |
| 8 | A JIT | the largest ceiling | possible with icount-style budgets, but the largest verification surface |

**3. Pre-decoded blocks.** After (1), DOOM still spends ~87 host instructions a
step, all of it in the fast loop itself: the fetch compare, a four-field decode
tag check, the `switch`, `write_x`'s x0 test and the history write, every step.
libriscv decodes a whole straight-line block once, executes from that array
with no per-instruction tag check, and adds the block's instruction count to the
counter when it enters the block, checking the limit only at block boundaries.
QEMU's icount does the same thing for its JIT and says how to keep it exact:
the budget is checked before the block runs, and when the block will not fit,
it is re-run one instruction at a time so the budget reaches zero exactly when
the timer is due. DoomV already has the budget -- `run_fast`'s `limit` -- so a
block runs whole only when `block.len <= limit - done`, and otherwise the loop
steps. A trap inside a block (a load that misses) knows its index, so the count
is exact there too. The history ring can then record one entry per block --
the block and how much of it ran -- and be expanded when crash.log or the
dashboard reads it, which also takes the ring's L1 misses off the hot path. For
the expansion to be exact, a block the ring still refers to has to stay alive
with the encodings it ran, even after its code is overwritten; blocks are
immutable once built, so keeping the last few thousand alive is enough. Invalidation is the risk the per-page
experiment in README.md already identified: a block must be dropped when its
bytes change, which the decode tag handles today for free. Checking the bytes of
a block's page against a per-page write generation -- bumped by the store cache
and by `write*` -- is the standard answer.

**4. The decode cache.** After (5), 1.9M of Linux's 2.0M remaining decode
misses are an entry evicted by another pc; the boot's code does not fit in
2^17 entries. At 2^19 entries misses fall 69%, to 0.64M, and the boot runs 7%
faster (56.2 to 60.2 MIPS, plain build, same crash.log) -- but at 72 bytes an
entry that is a 38 MB table. Folding high address bits into the index made it
worse (2.7M), so the direction is capacity, paid for by shrinking the entry: it
carries a mnemonic pointer only the dashboard needs and a second copy of the raw
word, and 32 bytes is within reach, which also halves the cache lines a lookup
touches. Worth doing together with (3), which changes what the cache holds.

**5. PGO.** Measured here, PGO + ThinLTO is 1.44x over ThinLTO alone on DOOM,
the same shape as README.md's 81.5 to 113. `pgo.py` exists; the point is only
that it is the build worth running, and that its profile should be retrained
after (1), since the hot code moves.

**6. Snapshots.** The Ubuntu desktop is twenty minutes of emulated time past
where `bench.py` stops, and a systemd boot is minutes every time. A snapshot --
registers, CSRs, RAM, every device's state, the timer, the step count, plus the
disk images and shared-folder state as inputs -- turns both into a load. It is
the practical accelerator with the largest effect on how the machine is used,
and determinism is what makes it checkable: boot to *N*, snapshot, restore, run
to *N + M*, and the crash.log must equal a straight run to *N + M*. QEMU's
record/replay is built the same way.

**7. Smaller host-code techniques**, all unmeasured here:

* *Superinstructions / macro-op fusion.* rv32emu's recent interpreter work fuses
  common sequences (a store and its pointer increment, runs of halfword loads
  and stores, libc's byte-copy and `strlen` loops) and reports 2.1-3.7x on its
  benchmarks, together with a contiguous block layout and chaining -- so most of
  that is (3) as much as fusion. For DoomV each fused pair must still count as two steps and write
  two history entries, and the second half must be able to trap on its own --
  so fuse only pairs whose second half cannot fault.
* *Tail-call threaded dispatch* with Clang's `[[clang::musttail]]`: each handler
  tail-calls the next with the hot state in argument registers. CPython 3.14
  adopted it; once a compiler regression was accounted for, the measured gain
  is about 1-5% over computed goto. Clang-only, so it would need a fallback for
  GCC.
* *BOLT*, the post-link layout optimizer. Mostly an instruction-cache win for
  large binaries; the emulator's hot loop is small, so expect little.

**8. A JIT.** README.md's reasons for not having one still hold. If it is ever
done, QEMU's icount is the existence proof that a translator can be exact about
instruction counts and interrupt timing, and the interpreter stays the oracle:
the JIT can be lock-stepped against it, instruction for instruction, the same
way the interpreter is held to Sail.

## What would break determinism

For completeness, the tempting things that do not keep the invariant:

* **More than one host thread executing the guest.** Even with one hart, any
  speculative or parallel execution must commit in guest order at guest counts;
  with more than one hart, the interleaving would have to be scheduled on
  instruction counts (a fixed quantum per hart), never left to the host.
* **Timers or device completion driven by host time.** Disk reads today are a
  synchronous `fread` on the CPU thread at the notify, which is exact. Reading
  ahead on another thread is fine; *completing* early is not -- the result must
  still land at the instruction that asked for it.
* **Compiler flags that change arithmetic.** `-ffast-math`, `-march=native` with
  FMA contraction (README.md already rules this out), or anything else that lets
  the host's floating point differ between builds.
* **Batch sizes that depend on host speed.** `cpu_loop`'s burst budget is a
  constant, and the input checkpoints are on counts; a "run until 16 ms have
  passed" loop would reintroduce exactly what those removed.

## Adopting the prototype

1. `git apply performance/patches/fast-loop.patch` against `3771294` (add
   `--ignore-whitespace` if the sources and the patch disagree about CRLF), and
   decide whether to keep the switches. The A/B switch and the state dump are
   cheap and useful for checking later changes.
2. `python performance/bench.py doom --compare build/perf-baseline/riscv_doom.exe`,
   and the same for `linux` and `ubuntu`: the hashes must match.
3. `python tools/verification/lockstep_sail.py`. Lock-step keeps the old
   step-by-step loop, so this checks (4) rather than `run_fast` itself.
   That is also why (4)'s mask needs a review against every reader of
   `state_gen` before it lands.
4. `python scripts/ci.py`.
5. Retrain the PGO profile, since the hot code has moved.

## Sources

* QEMU, [TCG Instruction Counting](https://github.com/qemu/qemu/blob/master/docs/devel/tcg-icount.rst)
  -- the per-block budget and how it stays exact at timers and I/O.
* libriscv, [`cpu_dispatch.cpp`](https://github.com/libriscv/libriscv/blob/master/lib/libriscv/cpu_dispatch.cpp)
  and the [project README](https://github.com/libriscv/libriscv) -- pre-decoded
  execute segments, block-level instruction counting.
* rv32emu, [PR 775](https://github.com/sysprog21/rv32emu/pull/775) -- block
  layout, chaining and macro-op fusion in an interpreter, with determinism
  checked across builds.
* Rohou, Swamy, Seznec, [Branch Prediction and the Performance of Interpreters
  -- Don't Trust Folklore](https://inria.hal.science/hal-01100647v1), CGO 2015.
* Nelson Elhage, [Performance of the Python 3.14 tail-call
  interpreter](https://blog.nelhage.com/post/cpython-tail-call/) -- what
  `musttail` dispatch is worth once the baseline is right.
