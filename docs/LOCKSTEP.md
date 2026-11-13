# Lock-step with DoomV

DoomV is a RISC-V emulator. The Pico 2's RP2350 has two Hazard3 RISC-V cores.
Pico2Doom runs DOOM on those cores, so DoomV can run **the same machine
code** -- and be checked, frame by frame, against the real chip.

```
 Pico 2 (Hazard3)                      PC
 ┌──────────────────┐   USB    ┌────────────────┐  file  ┌──────────────────┐
 │ DOOM             │ ───────▶ │ picolink.py    │ ─────▶ │ DoomV            │
 │  clock, keys ────┼─ logged  │  record        │        │  same DOOM code  │
 │  each frame: ────┼─ hashes, │  (+ video      │        │  inputs replayed │
 │   picture, state,│  retired │   window)      │        │  hashes + counts │
 │   minstret       │  counts  └────────────────┘        │  compared; halts │
 └──────────────────┘                                     │  at a difference │
                                                          └──────────────────┘
```

## What is compared

DOOM is deterministic: given the same clock readings and key events in the
same order, the same code computes the same frames. Those two inputs reach
the engine through exactly two calls, `DG_GetTicksMs` and `DG_GetKey`
(`port/port_link.c`). The Pico logs every value they return; DoomV feeds
the same values back. Then, at the end of every frame, both sides produce:

| | |
|---|---|
| frame hash | of the 64,000-byte picture DOOM drew |
| game-state hash | every monster's position, momentum, state and health, every sector's floor, ceiling and light, the player, both random number generators |
| retired instructions, game logic | `minstret` over `G_Ticker` |
| retired instructions, rendering | `minstret` over the BSP walk, floors and ceilings, sprites and status bar |

DoomV halts at the first frame where any of the five differs -- or the
first time DOOM asks for an input the Pico did not read at that point --
writes `crash.log` (its whole machine state) and `picolink_doomv.ppm` (the
frame as it drew it), and from there its own instruction-by-instruction
lock-step against Sail can take over.

## Why at retirement

Hazard3 is pipelined: at any moment it has instructions fetched, decoded or
executing that may never complete -- squashed by a branch, a trap or a stall.
Only *retired* instructions are architectural, and `minstret` counts exactly
those. DoomV executes one instruction at a time, so its `minstret` means the
same thing, and the two counts can be compared directly. Per-instruction
lock-step with the real chip is not possible (no trace port, and 150 million
instructions a second against ~1 MB/s of USB), so this compares the retired
count over every frame instead.

For the counts to be comparable:

- **Same instructions.** The firmware and the DoomV guest are built by the
  same compiler with the same flags. `tools/compare_isa.py` disassembles
  both and confirms that all 890 shared functions (83,791 instructions) are
  instruction-for-instruction identical; only the platform backends and the
  C library's system calls differ.
- **No linker relaxation.** Relaxation shortens address sequences only for
  data that lands near `gp`, which differs between the images
  (`port_riscv_lockstep_flags`).
- **Only shared code is counted.** The counted windows are engine
  functions; whatever runs inside `DG_GetTicksMs`/`DG_GetKey` (USB on the
  Pico, the replay device on DoomV) is measured and subtracted. The
  `minstret` read is an inline `csrr`, so the window holds nothing else.
- **No interrupts on core 0.** USB runs on core 1, and in link mode sleeps
  spin instead of waiting on a timer interrupt.
- **Same heap.** The recording starts with the Pico's zone size and DOOM's
  arguments; DoomV gives DOOM exactly the same, so allocation decisions
  match.

## Running it

1. Build the DoomV guest: `cmake -S doomv -B build/doomv -G Ninja
   -DCMAKE_TOOLCHAIN_FILE=$PWD/doomv/riscv-none-elf.cmake && cmake --build build/doomv`
2. Record from the Pico (it reboots into link mode, recording from DOOM's
   first clock reading):
   `python tools/picolink.py record build/run.p2dl --video 4`
3. Replay in DoomV -- live, while recording, or afterwards:
   ```
   riscv_doom build/doomv/doom1.wad build/doomv/pico2doom-doomv.elf \
       -march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -picolink=build/run.p2dl
   ```
   DoomV reports every 175 frames in step, or the first difference.

`--video N` streams every Nth frame of the Pico's screen over USB into a
window beside DoomV's (it costs the Pico ~60 ms per streamed frame), and
keys typed in that window are played on the Pico.

## Input from the Pico and from the PC

Core 1 puts button presses *and* keys from the PC (`picolink.py play` or
`record --video`) into one input buffer; DOOM's `DG_GetKey` takes them from
there. Every key DOOM reads is recorded, whichever way it came, so DoomV
replays PC and button input alike and stays in step. Each key is also sent
to the PC as an `INPUT` record with its source -- in every mode, not just
when recording -- so a press on the Pico shows up on the PC.

## Real hardware against DoomV

A 40-second recording from a real Pico 2 -- boot, title screen, the DEMO1
attract demo on E1M5 -- replayed in DoomV:

| | |
|---|---|
| frames | 897 |
| pictures and game state identical | **897 of 897** |
| retired-instruction counts identical | **896 of 897** -- ~910 million instructions of DOOM |
| the exception | frame 161, where the demo starts and E1M5 loads: DoomV retired 64 more instructions in game logic, with the same picture and state |

`PICOLINK_HASHES_ONLY=1` is DoomV's diagnostic mode for this: it reports
retired-count differences but halts only on a picture or state difference.
The 64 instructions come from a one-off path at level load; the next step is
to split the game-logic counter into finer regions (level load, the
per-system tickers) on both machines to name the code. Candidates are the
places the shared code reaches the C library's platform layer -- pico-sdk
gives newlib multicore-safe lock hooks where DoomV's newlib has none.

## What it has found so far

Before a single recording came from hardware, lock-step against a recording
from the PC simulator found two bugs in DoomV's RV32 mode, both now fixed in
DoomV:

1. **Addresses were not truncated to 32 bits.** RV32 values sit
   sign-extended in DoomV's 64-bit registers, so every address at or above
   `0x80000000` -- where guest RAM has been since DoomV booted Linux --
   faulted. Fixed in `mmu_translate`.
2. **Bit-manipulation instructions used RV64 semantics.** `rol`, `ror`,
   `rori`, `clz`, `ctz`, `cpop`, `rev8`, `bset`/`bclr`/`binv`/`bext`,
   `pack` and `zext.h` computed on 64 bits. GCC uses `zext.h` for every
   `unsigned short`, so DoomV drew DOOM with a few hundred texels wrong per
   frame -- caught as a picture-hash mismatch at frame 38 with the game
   state still identical. `tools/rv32check` now checks 15,360 results (60
   instruction forms × 256 operand pairs) against the RISC-V spec: 1,641
   were wrong, 0 now.

Recording format: `port/port_link.h`. DoomV side: `src/pico_link.cpp`
(DoomV branch `pico2doom-link`).
