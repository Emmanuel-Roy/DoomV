# Fitting DOOM into a Pico 2

The RP2350 has **4 MB of flash** and **520 KB of SRAM**. Stock doomgeneric
wants about **920 KB of RAM** for the biggest shareware map, and the
shareware WAD alone is bigger than the flash. Everything below was measured,
not estimated: with the firmware's linker output (`tools/membudget.py`,
printed after every build) and with the PC simulator built 32-bit, so its
structures are the RP2350's size (`sim/`).

## Flash: 4,096 KB

| | KB |
|---|---:|
| `DOOM1.WAD` as shipped | 4,098 -- 2 KB more than the whole chip |
| packed by `tools/wadpack.py` | **3,299** |
| firmware (code + constant data) | 470 |
| free | 328 |

The packer removes what a Pico without a speaker cannot use -- sound effects,
PC-speaker sounds, music and the synth banks (782 KB) -- keeps their
directory entries as empty lumps so name lookups still work, stores
identical lumps once, and aligns every lump to 4 bytes. It is *not*
compressed, on purpose: DOOM reads lumps straight out of the memory-mapped
WAD without copying them, so every graphic costs no RAM. A compressed lump
would have to be unpacked into RAM, the one thing there is less of.

## SRAM: 512 KB (+ 8 KB stack)

| | Stock doomgeneric | Pico2Doom |
|---|---:|---:|
| 32-bit copy of every frame (`DG_ScreenBuffer`) | 250 KB | **0** -- sent from the 8-bit frame |
| static data (`.data` + `.bss`) | 397 KB | **235 KB** |
| `malloc` (WAD directory, ...) | 35 KB | **0.1 KB** |
| left for DOOM's zone (heap) | -- | **269 KB** |

Where the static memory went: the renderer's fixed-size arrays sized from
what the maps really use, constant tables kept in flash, netgame buffers for
the 4 players DOOM actually has (see [patches/README.md](../patches/README.md)
for each patch and what it saves).

What is left in RAM (the largest): the 8-bit screen (62 KB), visplanes
(36 KB), openings (12 KB), light tables (11 KB), `viewangletox` (8 KB),
sprite and wall-segment lists (14 KB), the LCD's DMA buffers (2.5 KB),
USB buffers (5 KB) and core 1's stack (2 KB).

## Which maps fit

Each map was run in the simulator with exactly the Pico's 269 KB zone, at
Ultra-Violence (most monsters), teleporting through every sector and
spinning for 4,000 frames with the monsters awake (`doom_sim -explore`), and
again with 8 KB less as a margin:

| Map | Zone needed | On a Pico 2 |
|---|---:|---|
| E1M1 Hangar | < 247 KB | **fits** |
| E1M4 Command Control | ~250 KB | **fits** |
| E1M5 Phobos Lab | ~264 KB | **fits** (also DEMO1, the benchmark) |
| E1M8 Phobos Anomaly | < 247 KB | **fits** |
| E1M9 Military Base | < 247 KB | **fits** |
| E1M2 Nuclear Plant | ~300 KB | too big |
| E1M3 Toxin Refinery | ~320 KB | too big |
| E1M7 Computer Station | ~320 KB | too big |
| E1M6 Central Processing | ~400 KB | too big |

Choosing a map that does not fit shows a "not enough RAM for E1Mx" screen;
the title screen's demo loop skips the demos on those maps. The rest would
need level geometry moved into flash and DOOM's line structure split in two
-- the route rp2040-doom took -- or an RP2350 board with PSRAM.

## Two memory problems the simulator found

- **Fragmentation.** With 91 KB free, E1M1 still failed: DOOM wanted 32 KB
  in one piece to build a multi-patch wall texture. Patch 06 assembles such
  columns one at a time instead, and the largest allocations left are the
  level's own arrays.
- **The screen melt** needs a 64 KB buffer for a moment between levels
  (stock: 192 KB). It is borrowed only if the zone can spare it (patch 08);
  otherwise the new screen just appears.

## Borrowing memory for the display

Core 1 can send a frame to the LCD while core 0 draws the next one -- but
only from a copy, and a whole frame is 64 KB. Rather than reserve it (which
would cost E1M4 and E1M5), `port/port_video.c` *borrows* it from the zone as
a cache block: the zone takes it back whenever DOOM needs the room. The whole
frame is tried, then half, then a quarter; DOOM waits only for the rows that
are not copied. Measured share of each frame sent without waiting:

| E1M1 | E1M4 | E1M5 | E1M8 | E1M9 |
|---:|---:|---:|---:|---:|
| 98% | 25% | 5% | 98% | 50% |

Because the borrowing is shared code, DoomV borrows and loses the block at
the same moments, and its heap stays identical to the Pico's.

## Reproducing the numbers

```sh
cmake -S sim -B build/sim -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/sim/zig-x86.cmake
cmake --build build/sim
cd build/sim
./doom_sim -wad doom1.wad -zone 269 -fastclock -explore -frames 4000 -- -warp 1 5 -skill 4
./doom_sim -wad doom1.wad -fastclock -explore -frames 2000 -zonereport -- -warp 1 6 -skill 4
```
