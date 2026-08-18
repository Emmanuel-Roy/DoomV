# DoomV Restructure Plan

Splitting the current single-blob core into the layout below, plus a punch list
of things this restructure surfaces that aren't decided yet.

## File layout

```
main.cpp            entry point
doom_system.h/.cpp   top-level: owns decoder + core + memory + registers + debugger + gui, run loop
memory.h/.cpp         guest RAM, WAD/ELF loader, full MMIO bus dispatch
extensions.h          enabled-extension table (currently I, M, A)
registers.h/.cpp      x0-x31 + PC + CSR storage, read/write accessors, trace history ring buffer
riscv_decoder.h/.cpp  raw instruction word -> decoded instruction struct, extension gate, dispatch to core
riscv_core.h/.cpp     one execute function per extension, operating on Registers + Memory
debugger.h/.cpp       breakpoint list, halt condition, crash log dump
gui.h/.cpp            SDL window/renderer, framebuffer scaling, register/memory/trace dashboard
```

On the guest side (compiled to RV32IMA, not part of the emulator itself):
doomgeneric + two new platform files, `doomgeneric_doomv.c` (the 5-function
porting layer) and `w_file_doomv.c` (WAD backend reading straight out of
linked-in guest memory, see below).

Flow: `main` launches the GUI and loads Doom into `memory`, then loops:
`doom_system` fetches a word via `memory`, `riscv_decoder` looks it up in a
classification table to determine which extension it belongs to, checks
that extension against `extensions.h`, and — if enabled — calls the
matching function in `riscv_core`. `doom_system` services MMIO and
refreshes `gui` each frame.

So the enabled/disabled gate lives in the decoder, not the core: `riscv_core`'s
`exec_32I`/`exec_32M`/`exec_32A` functions can assume they're only ever
called for instructions that were already classified and cleared. The
decoder is also where "unimplemented extension" fails loud (see below).

`registers.*` is state, not logic — `riscv_core` is the only thing allowed
to mutate it (general regs through `exec_32I`/etc., CSRs later through
`Zicsr` execute functions once that lands). Everything else, including
`gui`, only ever gets a read-only view of it. This also replaces the old
`friend class` reach-through GUI used to need into the core.

## Class breakdown

Interface-level only — no implementation, just what each class owns and
exposes.

- **`Extensions`** (`extensions.h`) — no class, just data: a `constexpr`
  table of which extensions are enabled (`I`, `M`, `A` = true; `C`,
  `ZICSR`, `V` = false for now).

- **`Registers`** (`registers.h/.cpp`) — pure state, no execution logic.
  - Owns: `uint32_t x[32]`, `uint32_t pc`, a flat `uint32_t csr[4096]`
    (reserved, unused until Zicsr lands), and the ~4096-entry
    `{pc, instr}` trace ring buffer.
  - Mutating methods (`write_x`, `set_pc`, CSR writes, `record_history`)
    are only ever called by `RiscvCore`.
  - Read-only accessors (`read_x`, `get_pc`, history iteration) are what
    `Gui` and `Debugger` use — neither gets a mutable reference.

- **`Memory`** (`memory.h/.cpp`) — owns the full bus, resolving the "memory
  vs MMIO split" item below: RAM, framebuffer, palette (pending the
  framebuffer-format decision), input register, and the instruction-count
  tick register all live here.
  - `read8/16/32`, `write8/32` — address dispatch (RAM range vs each MMIO
    region).
  - `load_wad(bytes, address)`, `load_elf(path)` (or flat-binary loader,
    per the still-open ELF-vs-flat decision) — placement into RAM before
    boot.
  - Owns nothing about *how* to interpret MMIO writes beyond storing them;
    e.g. it doesn't know about SDL, it just holds the framebuffer bytes for
    `Gui` to read.

- **`DecodedInstruction`** (`riscv_decoder.h`) — plain struct, not a class:
  extension tag, `rd`/`rs1`/`rs2`, `imm`, raw `funct3`/`funct7`, `opcode`,
  and instruction length (2 or 4 bytes).

- **`Decoder`** (`riscv_decoder.h/.cpp`) — holds references to `RiscvCore`
  and `Registers` (set at construction by `DoomSystem`), no state of its
  own beyond that.
  - `DispatchResult decode_and_dispatch(uint32_t raw_instr)`: classifies
    via the opcode/funct7 table, checks `Extensions`, and — if enabled —
    decodes fields and calls the matching `RiscvCore::exec_32*`. Returns a
    small result (`{ bool illegal; }`) so `DoomSystem` can react without
    the decoder itself owning halt policy.

- **`RiscvCore`** (`riscv_core.h/.cpp`) — the actual execution logic, no
  owned state (operates on the `Registers&`/`Memory&` it's given).
  - `exec_32I(const DecodedInstruction&, Registers&, Memory&)`,
    `exec_32M(...)`, `exec_32A(...)` — one per enabled extension, matching
    the README's switch-case-over-function-pointers stance internally too.
  - Resolves the "PC advancement" item below: each `exec_32*` is
    responsible for leaving `pc` correctly set before it returns — either
    advanced by the instruction's length for straight-line code, or set to
    the branch/jump target. No separate auto-increment step anywhere else.

- **`Debugger`** (`debugger.h/.cpp`) — owned by `DoomSystem`, not part of
  the fetch/decode/execute path itself.
  - Owns: the loaded breakpoint address list, a `halted` flag.
  - `load_breakpoints(path)` — reads the startup config file.
  - `bool should_halt(uint32_t pc, bool instr_was_illegal)` — checked once
    per step by `DoomSystem`.
  - `dump_log(const Registers&, path)` — writes the ring buffer to
    `crash.log` on halt.

- **`Gui`** (`gui.h/.cpp`) — owns the SDL window/renderer/texture and the
  8x8 font table; everything currently in `Doom_System.cpp`'s
  `update_display`/`draw_char`/`draw_string`/`draw_rect`/`handle_input`
  moves here essentially as-is, just detached from CPU/bus state.
  - `render(const Registers&, const Memory&, const Debugger&)` — read-only
    views only, draws the framebuffer + register/trace dashboard.
  - `poll_input()` — returns raw key events; translating those into Doom's
    key constants is `Memory`'s or `DoomSystem`'s job (see the input key
    mapping TODO below), not `Gui`'s.

- **`DoomSystem`** (`doom_system.h/.cpp`) — the orchestrator. Owns one
  instance each of `Memory`, `Registers`, `RiscvCore`, `Decoder`,
  `Debugger`, `Gui`.
  - `bool init(wad_path, elf_path)` — loads WAD + guest binary into
    `memory`, initializes `gui`.
  - `void run()` — the loop: burst of `step()` calls, then service MMIO
    input from `gui.poll_input()`, refresh `gui.render(...)`.
  - `void step()` — fetch a word via `memory`, hand it to
    `decoder.decode_and_dispatch(...)`, check `debugger.should_halt(...)`
    afterward and react (freeze + `debugger.dump_log(...)` if so).

## Responsibilities / interfaces to nail down

- **DecodedInstruction struct** — shared shape the decoder hands the core.
  Needs at least: extension tag, rd/rs1/rs2, imm, raw funct3/funct7, and
  instruction length (2 vs 4 bytes) so something can advance PC correctly.
  Decide which file owns this type (probably `riscv_decoder.h`, included by
  `riscv_core.h`).
- **Decoder classification table** — needs a table keyed on opcode (and
  funct7 where the opcode is shared, e.g. `0110011` covers both 32I ALU ops
  and 32M MUL/DIV, distinguished by funct7) that maps to an extension tag.
  Confirmed: pure classification data (opcode/funct-bits -> extension enum),
  not function pointers. The decoder does a small `switch` on that enum to
  call `exec_32I`/`exec_32M`/`exec_32A`, consistent with the README's stance
  against function-pointer dispatch — a 3-4-way switch on extension tag
  doesn't carry the branch-prediction cost a full per-instruction pointer
  table would.
- **Extension gate ownership** — decoder checks `extensions.h` before
  calling into core, so a disabled extension never reaches `riscv_core` at
  all. Core functions don't re-check; they trust the decoder.
- **PC advancement / branch handling** — resolved in the class breakdown
  above: each `RiscvCore::exec_32*` sets `pc` itself before returning
  (advance-by-length or branch target), no separate step owns this.
- **Memory vs MMIO split** — resolved: `Memory` owns the full bus (RAM, FB,
  palette, input, tick register). `DoomSystem` doesn't do address routing
  itself.
- **GUI access to register state** — resolved by pulling register/CSR/PC
  storage out into `registers.*`: GUI takes a `const Registers&` to draw
  the dashboard, `riscv_core` holds the only mutable reference. No more
  `friend class` reach-through.
- **CSR storage shape** — decide the CSR backing store now even though
  Zicsr execution isn't wired up yet (extensions table has it off): a small
  fixed array indexed by the 12-bit CSR address is simplest, vs. a map.
  Reserve the space in `registers.*` so turning Zicsr on later is just
  adding an `exec_zicsr` in core, not a storage redesign.

## Debugging: breakpoints + halt log

- **Ownership**: lives in `doom_system`'s run loop, not `riscv_core` or
  `riscv_decoder` — stopping is a cross-cutting policy, not fetch/decode/
  execute logic. `doom_system` checks the halt condition once per step,
  between core executing and the next fetch.
- **Breakpoint types**: PC-address breakpoints, loaded from a config file at
  startup (a simple list of addresses — no in-GUI add/remove for now, that's
  a later addition if it turns out to be needed). Plus an always-on implicit
  breakpoint: any illegal/unimplemented-extension instruction from the
  decoder's gate, or any core-detected fault, halts automatically with no
  config needed.
- **The log**: replace the current `history[5]` ring buffer (moving to
  `registers.*`) with an always-on ~4096-entry ring buffer (PC + raw
  instruction word per step). Always recording, not just after a halt, so
  the first fault still has history leading up to it. Cost is trivial next
  to the 128MB guest RAM budget already in play.
- **On halt** (breakpoint hit or fault): dump the full ring buffer to a
  host-side text log file (e.g. `crash.log`), separate from the GUI's live
  trace panel. GUI keeps showing just the last few instructions at a
  glance; the file is the full post-mortem view.
- Ties directly into the existing "Illegal/unimplemented instruction
  behavior" TODO below (this *is* that failure mode) and the "minimal test
  harness" TODO (breakpoints + log are exactly what makes single-stepping
  through hand-assembled test instructions useful).

## Guest memory map (resolved)

Compact and contiguous, not the sparse `0x80000000`-based layout the
original single-file code had. MMIO first, framebuffer right after, RAM
grows up from there, WAD sits past the end of RAM. Defined in
`tools/doombuild/doomv_mmio.h` and `tools/doombuild/riscv.lds`:

```
0x10000000 - 0x10000FFF   MMIO (input reg, tick reg, headroom for later) - 4K
0x10001000 - 0x10040FFF   Framebuffer, 320*200*4 = 256000B, padded to 256K
0x10041000 - 0x11040FFF   RAM: .text/.data/.bss/heap/stack - 16MB
0x11041000 - 0x1243FFFF   WAD, placed by the host loader - 20MB
```

WAD region sized for the largest real WAD (Final Doom's
`plutonia.wad`/`tnt.wad`, ~18-19MB), not just the shareware/registered
ones (~4MB/~12-14MB) — picked deliberately, not by symmetry with the RAM
size like the first pass at this map was.

`__stacktop` = `0x11041000` (top of the RAM region, grows down); heap
grows up from `_end`/`_heap_start`, same converging pattern as before,
just sized realistically (16MB, ~4x original DOS Doom's actual 4MB
footprint) instead of an arbitrary 64MB reservation.

**Dependency**: `RV32IMAC_Core`'s reset PC is still hardcoded to the old
`0x80000000` in the not-yet-rewritten core file. Needs to change to
`0x10041000` when that file gets split into the new `riscv_core.*` design
— `start.S`/`riscv.lds` are already validated against the new address,
the CPU side just hasn't caught up yet.

## Things missing before this can boot anything

- **Illegal/unimplemented instruction behavior** — resolved by the
  breakpoints/halt-log system above: when `riscv_decoder` classifies an
  opcode as belonging to a disabled/unimplemented extension, that's an
  automatic implicit breakpoint — halt, dump the ring buffer to `crash.log`.
- **Toolchain choice — resolved.** xPack GNU RISC-V Embedded GCC
  (`riscv-none-elf-gcc` 15.2.0-1), installed at
  `C:\Users\royem\SWE\toolchains\xpack-riscv-none-elf-gcc-15.2.0-1`
  (outside any git repo — it's ~465MB, not something to ever commit). No
  Windows arm64 build exists, only win32-x64, running fine under this
  machine's x64 emulation once its `bin/` DLLs are resolvable (see the
  `ucrt64/bin` PATH fix from the SDL2 setup — same category of issue).
  As predicted, there's no exact prebuilt `rv32ima` multilib (`-print-multi-lib`
  shows `rv32im`/`rv32ia_zaamo_zalrsc` separately, nothing combined without
  `c`) — turned out not to matter even for linking: `ld`'s multilib
  matching accepted `rv32ima/ilp32` against the `rv32ia_zaamo_zalrsc/ilp32`
  prebuilt libc fine (IMA is a superset, our own code still gets real `mul`/
  `div` instructions, the precompiled libc just doesn't happen to use them
  internally). See the libc-linking bullet below — this ended up mattering
  a lot more than expected.
- **doomgeneric vendored** — added as a git submodule at
  `tools/doombuild/doomgeneric` (pointing at `ozkl/doomgeneric`), not a
  frozen copy, so it can be updated/pinned deliberately later.
- **Startup code** — since Zicsr isn't enabled, whatever boots the Doom
  binary can't rely on the toolchain's own `crt0.o` (it touches CSRs and
  defines its own conflicting `_start`). Solved with a hand-written
  `start.S`: zero `.bss`, set `sp`, call `main` (no `.data` copy needed,
  see the memory map section). Built with `-nostartfiles`, not `-nostdlib`
  — that distinction turned out to matter a lot, see below.
- **Libc — resolved, and it's not what the plan originally assumed.**
  Originally figured on hand-writing every libc function doomgeneric calls
  (`memcpy`, `strlen`, the whole `printf` family, `malloc`, ~50 symbols
  total from a real link attempt). Turns out unnecessary: swap `-nostdlib`
  for `-nostartfiles -specs=nosys.specs` and the toolchain's real
  `libc.a`/`libnosys.a` link in fine (see the toolchain bullet above for
  why the multilib mismatch doesn't block this). `-nostartfiles` drops
  only the toolchain's `crt0.o` (which we don't want anyway, see above) —
  everything else in libc, memcpy/strlen/printf/malloc included, comes for
  free. `_sbrk` (malloc's backing store) already works via `PROVIDE(end = .)`
  in `riscv.lds`, matching where newlib's `sbrk.c` looks. Full doomgeneric
  build (all ~80 engine sources + our 3 platform files) linked clean with
  **zero** undefined references after adding exactly one stub —
  `mkdir` (`libc_shim.c`), which isn't in `nosys.specs`'s default set.
  `_read`/`_write`/`_close`/`_lseek`/`_fstat`/`_isatty`/`_kill`/`_getpid`
  already exist as always-fail stubs from `nosys.specs` — fine for now
  (no real filesystem), revisit `_write` specifically if/when a UART
  gets added for debug output.
- **Floating point showed up, unexpectedly.** The pre-libc-fix link attempt
  surfaced soft-float intrinsics (`__mulsf3`, `__divsf3`, etc.) — something
  in doomgeneric's source uses `float`/`double` despite Doom's renderer
  being fixed-point by design (confirmed correct earlier). Likely a
  non-hot-path utility (config parsing, scale-factor math), not the
  renderer itself, but worth tracking down before assuming performance is
  unaffected — not blocking today since it links fine either way (GCC's
  soft-float library, not hardware F/D, handles it).
- **First real boot achieved — the title screen renders correctly.**
  Getting here took three real bugs, each found by actually running the
  built guest ELF against the host emulator and watching it (screenshots,
  a debug-output MMIO channel, and a memory watchpoint), not by
  inspection:
  1. `d_iwad.c`'s IWAD search goes through `fopen()`/`_open()`, not
     `w_file_doomv.c`'s `W_OpenFile` (that's only reached once the IWAD
     path is already resolved). Fixed with a real `_open`/`_read`/
     `_close`/`_lseek` in `libc_shim.c`, scoped to only succeed for
     `IWAD_NAME` opened read-only, so files that genuinely don't exist
     (config, saves) still correctly fall back to defaults.
  2. `Z_Malloc` failed allocating 64KB right after a 6MB zone had just
     succeeded — root cause invisible inside the precompiled `libc.a` (no
     newlib source shipped). Replaced the toolchain's `_sbrk` with our own
     simple bump allocator, same pattern the reference project uses.
  3. **The real one**: `riscv.lds`'s `.bss` rule (`*(.bss*)`) doesn't
     match `.sbss*` — a different name, not a suffix match. GCC/newlib
     emit many small-data `.sbss.*` sections; missing them meant the
     linker orphan-placed them *after* `_end`/`heap_start`, so the very
     first `malloc` call silently overlapped still-live globals (`drone`,
     `loop_interface`, `gametic`, ...). Found via a watchpoint on the
     corrupted global's address, catching the exact instruction that wrote
     the wrong value into it. Fixed by adding `*(.sbss*)`/`*(.scommon)` to
     the `.bss` rule (and `*(.sdata*)`/`*(.srodata*)` to `.data`/`.text`
     for the same reason, before they caused a quieter version of the same
     bug). Also found and fixed along the way: `DOOMGENERIC_RESX/RESY`
     were only being set via a header `#define` in `doomv_mmio.h`, which
     only affects files that `#include` it — `doomgeneric.c` (allocates
     `DG_ScreenBuffer`) and `i_video.c` (writes into it) don't, so they
     silently used doomgeneric's own 640x400 default while everything else
     assumed 320x200, corrupting the image with a stride mismatch. Moved
     to a global `-D` flag in the Makefile instead so every translation
     unit agrees.
- **Doom source: use doomgeneric, not raw linuxdoom.** Decided — doomgeneric's
  porting surface is one file, `doomgeneric_doomv.c`, implementing `DG_Init`,
  `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`, `DG_GetKey` (+ optional
  `DG_SetWindowTitle`). Sound is opt-in (`FEATURE_SOUND`), off by default —
  matches deferring the mixer to V-extension time. Entry point is
  `doomgeneric_Create(argc, argv)` then a loop calling `doomgeneric_Tick()`.
- **WAD loading — no syscall shim needed.** doomgeneric abstracts WAD access
  behind a `wad_file_class_t` vtable (`OpenFile`/`CloseFile`/`Read` function
  pointers, see `w_file.h`), implemented today by `w_file_stdc.c` via plain
  `fopen`/`fread`. Instead of faking POSIX `_open`/`_read`/`_lseek` through
  newlib, write one `w_file_doomv.c` implementing those 3 functions directly
  against a WAD blob at a fixed address in guest RAM. `memory.*`'s loader
  just needs to place the WAD bytes at that address before boot — no fake
  filesystem, no libgloss syscall layer for file I/O at all.
- **`_sbrk`/malloc still required.** WAD access no longer needs libc, but
  Doom's own allocator (`Z_Zone`/`Z_Malloc`) still calls `malloc` under the
  hood, so a real heap (`_sbrk` off a linker-defined `_heap_start`, same
  pattern as the reference port) is still needed regardless of doomgeneric.
- **IWAD auto-detection bypass** — `d_iwad.c` searches multiple paths/env
  vars looking for a WAD file, irrelevant on bare metal. Skip it by handing
  `doomgeneric_Create` a fixed, compile-time fake `argv` that points
  straight at the embedded WAD (e.g. `-iwad doom1.wad`), rather than trying
  to port the search logic.
- **Framebuffer format mismatch — open decision.** `DG_ScreenBuffer` is
  `DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4` — doomgeneric converts
  palette-indexed pixels to RGBA *inside* the guest binary before handing
  you a frame, unlike raw linuxdoom's 8-bit-indexed-plus-separate-palette
  model that the current `MMIO_FB` (8-bit) + `MMIO_PAL` split assumes. Need
  to decide: either drop `MMIO_PAL` as a separate region and widen `MMIO_FB`
  to 32bpp (matches doomgeneric's native output, simpler MMIO, but the
  palette-swap dashboard visualization goes away), or intercept before
  doomgeneric's RGBA conversion (keeps the palette-indexed model, more
  invasive patch into doomgeneric's internals, cuts against "easily
  portable" being the whole point of using it).
- **ELF vs flat binary loading** — GCC output is an ELF. Decide whether the
  loader parses ELF sections/segments into the right guest addresses, or
  whether the build step `objcopy`s down to a flat binary first (simpler
  loader, less flexible).
- **A minimal test harness** — compiling and booting all of Doom to find
  out if a single instruction was implemented wrong is a slow feedback
  loop. Worth having a way to hand-load a few bytes of hand-assembled
  instructions and single-step them through the dashboard before wiring up
  the full Doom binary.
- **Build system** — currently one Makefile, for the *emulator* (host C++).
  Splitting into this many files just means updating its source list for
  now; only worth reconsidering (e.g. CMake) if the file count keeps
  growing.
- **Guest build pipeline — resolved.** `tools/doombuild/Makefile`, separate
  from the emulator's own root `Makefile` (two independent build systems:
  one produces the emulator, the other the guest binary it loads). Lists
  all ~80 engine sources + the 3 platform files, `WAD=doom1|doom2|final`
  selects the target (default `doom1`) setting `WAD_LENGTH` accordingly,
  `make all` builds all three. No `make` binary exists on this machine
  yet though (checked `msys64` and the toolchain — not bundled with
  either) — validated the Makefile's build recipe by running the
  equivalent `riscv-none-elf-gcc` invocation directly instead. Full build
  linked clean, see the libc bullet above.
- **Input key mapping** — `DG_GetKey` expects Doom's own key constants
  (`KEY_RIGHTARROW` etc.) plus pressed/released events, not a raw SDL
  keysym. Current `handle_input()` just stores one raw key into
  `key_register`, which doesn't match. Needs a translation table
  (SDL keysym -> Doom key constant) and probably a small event queue, since
  `DG_GetKey` is polled once per tick and a single "current key" register
  can miss fast taps.
- **`DG_SleepMs`/`DG_GetTicksMs` semantics** — "sleep" doesn't mean much for
  a guest CPU inside an emulator. Should spin reading the tick MMIO register
  rather than actually stalling the host.
- **Halt/crash UX** — when the breakpoints/halt-log system fires, does the
  emulator process exit, or does the GUI stay up showing register/memory
  state for inspection? Log-and-halt only helps if something's still alive
  to look at afterward. Leaning toward: stay up, freeze execution, keep
  dashboard live.
- **Timing model — decided: instruction-count based, not wall-clock.** The
  MMIO tick register advances based on how many instructions the core has
  executed, not real elapsed time, so Doom's 35 tics/sec pacing (and
  `DG_GetTicksMs`) is deterministic/reproducible run-to-run rather than
  tied to host speed (unlike the reference port, which uses real time via
  a 70Hz video-tick counter). Still needs an actual instructions-per-tick
  constant picked once the core's real throughput is known.

## Explicitly out of scope for this pass

- **C extension** — deferred; not required to boot Doom, only a code-size
  optimization. Revisit once the core works if the toolchain forces it
  anyway.
- **Zicsr / CLINT** — deferred; the reference port boots Doom with no
  interrupts at all (polls a tick counter instead), so trap/CSR machinery
  isn't on the critical path.
- **V extension** — a later, deliberate addition to a couple of hot
  routines (audio mixing, framebuffer blit, screen wipe), not a general
  engine, and only after I+M+A are solid.
- **F/D (floating point)** — Doom's renderer is fixed-point by design, no
  floating point in the hot path.
