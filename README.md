# DoomV

A RISC-V CPU, built from scratch in C++, that boots bare-metal DOOM.

No OS, no Linux, no existing core as a reference implementation — just an
instruction decoder, a register file, a memory bus, and enough of the RV64GC
spec (plus Zicsr, Zifencei, and the V vector extension) to run a real game.

<img width="1919" height="1079" alt="image" src="https://github.com/user-attachments/assets/92adbcb6-cfe2-464e-8a09-8b817151a260" />

## Why

My Gameboy emulator was done, I'd taken a chunk of computer architecture
coursework by that point, and I had a month free before starting a co-op.
I wanted a project that would actually make me use that coursework instead
of just having sat through it, and "build a CPU and see if it can run Doom"
seemed like the right amount of stupid.

The original goal was small on purpose: get bare-metal DOOM booting, and if
it works, stop — nothing else needs to be implemented to call it done. That
happened a while ago. Everything past that point (F/D, C, V, formal
verification against a reference simulator) is stuff I kept going on because
it was fun, not because the project needed it.

## What's actually implemented

DoomV's default target is `RV64GC` plus `Zicsr` and `Zifencei` — i.e.
`RV64IMAFDC_Zicsr_Zifencei` — with the `V` (vector) extension on top,
opt-in since it's not needed to boot Doom itself.

| Extension | Status |
|---|---|
| I (base integer) | ✅ |
| M (mul/div) | ✅ |
| A (atomics) | ✅ |
| C (compressed) | ✅ |
| F / D (single/double float) | ✅ |
| Zicsr | ✅ |
| Zifencei | ✅ (no-op — see below) |
| V (vector) | ✅, off by default |

Every extension is a runtime toggle, not a compile-time one — pass
`-march=rv64imafdc_zicsr_zifencei` (the default), add a `v` for vector
support, or trim it down to something like `rv32ima` if you want to see it
fail in more interesting ways. `fence.i` is implemented as a genuine no-op
rather than being unsupported: there's no instruction cache here to
invalidate, since every fetch reads straight out of live guest memory, so
the correct emulation of "make sure instruction fetches see recent stores"
is to just not need to do anything.

## Correctness

I didn't want "it draws pixels that look like Doom" to be the bar for
correct. So partway through, I built [spike](https://github.com/riscv-software-src/riscv-isa-sim)
(the reference RISC-V ISA simulator) from source as a golden model, and ran
DoomV against the official [riscv-arch-test](https://github.com/riscv-non-isa/riscv-arch-test)
suite — the same compliance tests real silicon gets validated against —
comparing signature dumps instruction-for-instruction.

That process actually found real bugs: FP results that should've been the
canonical quiet NaN were leaking NaN payloads straight from the host FPU,
and one float-to-unsigned-64 conversion had undefined behavior on inputs at
the top of the range. Both are fixed. The full I/M/A/C/Zifencei suite
currently passes 115/127 applicable tests, with the remaining 12 being
tests for sub-extensions (Zcb, Zbb, Zba) DoomV was never trying to
implement in the first place. F/D passes the applicable suite as well. V
has no formal arch-test suite upstream yet, so it's cross-checked by hand
instead — breakpoint, full register/memory dump, compare against spike.

The harness lives in `tools/vtest/` if you want to see how any of that
works or run it yourself.

## Code layout

```
src/
  main.cpp             entry point, CLI flags, wires everything together
  doom_system.*         top-level system: owns decoder, registers, memory, gui
  memory.*              guest RAM, WAD/ELF loader, MMIO bus (framebuffer, input)
  registers.*           x0-31, f0-31, v0-31, PC, CSRs, and the trace history ring
  extensions.*           the enabled-extension table + -march= parser
  riscv_decoder.*        instruction word -> decoded struct, extension gate, dispatch
  riscv_core.hpp          shared declarations for each extension's execute function
  extensions/            one file per extension (ext_i.cpp, ext_m.cpp, ext_v_*.cpp, ...)
  debugger.*             breakpoints, halt conditions, crash/signature dumps
  gui.*                  the SDL window, framebuffer scaling, and the debug dashboard
```

Each extension owns its own decode + execute logic in its own file under
`src/extensions/` — nothing shared gets fought over, and adding a new
extension (which happened more than once) never means touching a giant
switch statement that also handles six other things.

## Building & running

```
make
./riscv_doom.exe tools/doombuild/DOOM1.WAD tools/doombuild/doomv-free.elf
```

`DOOM1.WAD` (the shareware IWAD) is the only WAD checked into this repo —
it's free to redistribute. Point it at your own `DOOM.WAD`/`DOOM2.WAD` if
you own a copy, and rebuild the guest ELF from `tools/doombuild/` to match.

Useful flags:
- `-march=rv64imafdc_zicsr_zifencei` — override the enabled extension set
- `-break=<hex_pc>` — halt and dump full CPU state at a given PC
- `-sig=<hex_begin>:<hex_end>` — dump a memory range on halt (what the
  arch-test harness uses to pull signatures)

Key bindings live in `controls.json` if you want to remap them.

The guest side — the actual Doom binary that runs *on* this CPU — is built
separately in `tools/doombuild/`: a cross-compiled `doomgeneric` with a
small platform layer (`doomgeneric_doomv.c`, `w_file_doomv.c`, a libc
shim) that talks to DoomV's MMIO instead of a real OS.

## Design notes

The core dispatches on a plain switch statement rather than a table of
function pointers. I went in assuming function pointers would be the
"proper" approach, but it turns out projects like QEMU deliberately avoid
that pattern — indirect calls through a function pointer table are worse
for branch prediction than a switch the compiler can reason about and
group cases in. So the core is, structurally, uglier than I'd like and
faster than the elegant version would've been.

Atomics were implemented even though a single-hart bare-metal target never
strictly needs them, because toolchain-emitted code (and doomgeneric's own
init sequence) assumes they exist. In practice every load/store here is
already atomic by construction, so the A extension mostly just needed to
exist and decode correctly.

## Sources

- [riscv-card](https://github.com/jameslzhu/riscv-card) — the reference
  sheet I built the initial decoder off of.
- [knazarov/rve](https://git.knazarov.com/knazarov/rve/) — not code I
  read, but the project that first showed me what set of extensions I'd
  actually need to get something like this booting.
- [riscv-opcodes](https://github.com/riscv/riscv-opcodes) — the
  authoritative encoding tables the V extension was implemented from.
- [spike](https://github.com/riscv-software-src/riscv-isa-sim) and
  [riscv-arch-test](https://github.com/riscv-non-isa/riscv-arch-test) —
  the reference simulator and compliance suite used for verification.
