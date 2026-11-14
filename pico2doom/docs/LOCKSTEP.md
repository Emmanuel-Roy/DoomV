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
lock-step against Sail can take over. [DIVERGENCE.md](DIVERGENCE.md) covers
what can make the two split and how to read the report.

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

- **Identical builds.** The firmware and the DoomV guest are built by the
  same compiler with the same flags, and every firmware build also builds the
  guest and checks them (see *Identical builds* below).
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

## Identical builds

Every firmware build (`cmake --build build/firmware`) also builds the DoomV
guest and runs `tools/compare_isa.py`, which fails the build unless every
function DOOM can reach from a counted window -- 422 functions, ~51,700
instructions, down through newlib and libgcc -- is the same machine code in
both images: same opcodes, same registers, same callees. What it took:

| Difference found | Fix |
|---|---|
| pico-sdk `--wrap`s `malloc`/`free`/`realloc` (with a mutex) and `printf`/`puts`/`vprintf` (through its stdio drivers), also inside newlib | the firmware defines `pico_malloc` and `pico_stdio` itself, without the wraps (`firmware/CMakeLists.txt`) |
| system calls (`_sbrk`, `_write`, ...) from pico-sdk on one side, a shim on the other | one shared `port/port_libc.c`; console output is platform code, measured and subtracted |
| pico-sdk's soft-float routines replace libgcc's | `pico_set_float/double_implementation(compiler)` |
| `-O3` from the SDK's release flags on some shared files | `port_riscv_lockstep_flags` pins `-O2` and no relaxation on every shared file |
| the zone started with power-on garbage on the Pico, zeroes on DoomV | shared code zeroes the zone and every piece of heap before use |
| newlib's `memset`/`memmove` align to 16 bytes, so their instruction count depended on where data landed in each image (addresses agree mod 4, not mod 16) | shared `memset`/`memmove` in `port_libc.c` that look only at alignment mod 4 |

A `minstret` self-test (`port/port_instret_test.c`) runs at start-up on both
machines and goes into the recording: reads around back-to-back CSRs, loads,
load-use stalls, divides, taken branches and stores give the same counts on
the real Hazard3 as on DoomV.

## Drilling down

When a frame's retired counts differ, build both with `-DPORT_FUNCPROF=ON`:
the engine is compiled with `-finstrument-functions`, and constant-cost,
branch-free hooks (`port/port_funcprof.c`) charge each stretch of retired
instructions to the function running -- the same hooks on both machines, so
they cannot perturb the comparison. Large game-logic windows are dumped per
function; the Pico's dump travels in the recording, so one DoomV log holds
both, and `tools/funcprof_diff.py` names the functions that differ.

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
window beside DoomV's, and keys typed in that window are played on the Pico.
The video is a half-size copy (160x100) that core 1 sends on its own USB
port while it sends the frame to the LCD, so it costs DOOM nothing. If USB
is busy, a row is dropped rather than waited for. Earlier firmware sent full
frames from core 0 on the link port, which held DOOM up for about 60 ms per
frame.

## Co-running live

`python tools/picolink.py corun [--warp E M]` runs the whole loop at once:

1. The Pico is rebooted into link mode, and its recording goes to
   `build/corun.p2dl`.
2. DoomV is started on that file (`PICOLINK_ACK=1`) and replays it as it
   arrives, printing `picolink-ack N` after each frame that matched.
3. Each ack lets the Pico run on: `G n` on the link port allows it to finish
   frames up to `n`, ack + `--ahead` (default 2). The Pico waits in
   `backend_link_check`, after the frame's record has gone out and outside
   every counted window. The clock values DOOM reads after waiting are
   recorded like any others, so pacing changes nothing DoomV compares.
4. At a difference, DoomV halts with `crash.log` and `picolink_doomv.ppm`, and
   picolink sends `H frame`. The Pico stops after the frame it is on and
   shows where DoomV diverged.

Measured on the real Pico 2: **a locked 30 fps** (29.9 to 30.0 every second
for 56 s) on E1M1, with video on, and every one of the 1,792 frames identical
in DoomV. With DoomV's window open it was the same: 1,492 of 1,492 frames,
29.9 fps average and minimum.

Two things limited the first version to about 10 fps:

- **picolink's reads.** `port.read(65536)` with a 0.2 s timeout handed the
  recording to DoomV in fifth-of-a-second chunks. It now reads what has
  arrived.
- **DoomV.** It ran the RV32 guest only on its slow path, because its fast
  path was RV64-only (51 MIPS). It now runs RV32 there too, at about 160
  MIPS: integer results are computed on 32 bits and sign-extended, exactly
  as its RV32 slow path does. The guest's own platform code had also been
  built at `-O0`. Presenting a frame alone cost as many instructions as
  DOOM's whole frame. Replaying the 864-frame recording above still matches
  on every frame, retired-instruction counts included.

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
| | First run | With identical builds |
|---|---:|---:|
| frames compared | 897 | 864 (boot, title, DEMO1 on E1M5) |
| pictures and game state identical | all | **all** |
| retired-instruction counts identical | 896 (one frame off by 64) | **all -- strict mode, no exceptions** |

A run starting on E1M1 (`picolink.py record --warp 1 1`) also matched in
strict mode on every one of its 916 frames: ~1.57 billion retired
instructions of DOOM, the same on the real chip and in DoomV.

The first run's one-frame difference, at DEMO1's start, took the
drill-down to find: first pico-sdk's malloc/printf wrappers, then newlib's
16-byte-aligning `memset` -- both now gone (*Identical builds*).
`PICOLINK_HASHES_ONLY=1` is DoomV's diagnostic mode for such hunts: it
reports retired-count differences and halts only on a picture or state
difference.

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
