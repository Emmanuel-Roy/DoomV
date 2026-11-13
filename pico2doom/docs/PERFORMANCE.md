# Performance

## How it is measured

Each phase of a frame is an engine function the port wraps (see
`PORT_WRAPS` in `port/port.cmake`, and `port/port_perf.c`): the wrapper reads
the Hazard3 cycle counter (`mcycle`) before and after. The engine itself is
not edited, and the cost is two CSR reads per phase per frame.

Once a second the Pico prints, on its USB console and in the 40 rows under
the picture:

```
fps 8.2 | frame 121.7 ms (max 140) | tic 3.1 bsp 15.0 planes 5.8 sprites 0.7 hud 0.6 lcd 95.5 | zone 234/271 KB (peak 236)
```

| Field | Meaning |
|---|---|
| `fps`, `frame`, `max` | frames per second; average and slowest frame time |
| `tic` | game logic (`G_Ticker`): monsters, physics, doors -- 35 tics per second of game time |
| `bsp` | walking the map's BSP tree and drawing walls (`R_RenderBSPNode`) |
| `planes` | floors and ceilings (`R_DrawPlanes`) |
| `sprites` | monsters, items, the weapon, masked walls (`R_DrawMasked`) |
| `hud` | the status bar (`ST_Drawer`) |
| `lcd` | converting the 8-bit frame through the palette and sending it over SPI |
| `zone` | DOOM's heap: live data / size, and the peak so far |

`python tools/picolink.py console --csv perf.csv` logs every line as a table
row and prints the average/min/max fps at the end.

**Benchmark:** `python tools/picolink.py bench` (or hold Fire while powering
on) runs DOOM's own `-timedemo demo1`: the attract-mode demo on E1M5, played
as fast as the machine can draw. DOOM reports `timed N gametics in M
realtics`; fps = N × 35 / M. This is the classic number DOOM ports are
compared by.

## First hardware run (Pico 2, 150 MHz, first firmware)

Measured on a real Pico 2 running the title-screen demo (DEMO1, E1M5):

| Phase | ms per frame |
|---|---:|
| game logic | 3.1 - 3.7 |
| walls (BSP) | 9 - 17 |
| floors and ceilings | 4 - 7 |
| sprites | 1 - 5 |
| status bar | 0.5 |
| **LCD transfer** | **95.5** |
| **frame** | **~122 (8 fps)** |

DOOM itself takes ~25 ms a frame -- fast enough for ~40 fps. The frame
was waiting on the display: one 320×200 frame at 16 bits is 1.02 Mbit,
and 95 ms means the SPI ran at ~12 MHz. The cause: the RP2350's peripheral
clock stays on the 48 MHz USB PLL unless it is moved, which caps SPI at a
quarter of that. The firmware now runs `clk_peri` from the 150 MHz system
clock, for a 37.5 MHz SPI clock -- ~27 ms per frame, an expected ~19 fps.
*(Confirmed on hardware: 37.5 MHz, see below.)*

**Benchmark on the first firmware:** `timed 5026 gametics in 21655 realtics`
-- **8.1 fps** for the whole of DEMO1, which completed on the real chip with
the heap peaking at 236 of 271 KB. During it DOOM's own work was ~17 ms a
frame (logic ~1, walls ~8, floors ~7, sprites ~1); the rest was the LCD.

## The second core

Core 1 now does all the I/O (`firmware/core1_io.c`): it sends frames to the
LCD, scans the buttons and runs USB, while core 0 runs only DOOM. A frame is
sent from a copy borrowed from DOOM's heap when there is room (see
[MEMORY.md](MEMORY.md#borrowing-memory-for-the-display)): on E1M1 and E1M8
98% of each frame goes out while DOOM is already drawing the next one, so the
frame time drops from *render + transfer* towards *the longer of the two*.
The read-out shows both sides: `lcd` is how long core 0 waited, and
`core1 lcd` how long core 1 took to send the frame.

Measured on the real Pico 2 with this firmware (title-screen demo, E1M5 --
the tightest map, where only part of each frame fits in a copy):

| | First firmware | Now |
|---|---:|---:|
| SPI clock | 12 MHz | **37.5 MHz** |
| core 0 waiting for the LCD | 95 ms | **22 ms** |
| core 1 sending a frame | -- | 30 ms |
| **frames per second** | **8** | **23.7** average, 29 peak |

On E1M1 and E1M8, where the whole frame fits in a copy, core 0 does not wait
for the LCD at all; the frame rate is then set by the 30 ms transfer.

## A locked 30 fps (250 MHz)

The firmware now runs at 250 MHz (`DOOM_SYS_MHZ`). The flash is retimed
first, at `clk_sys/3` (83 MHz) with the read sample point kept near the
stock 6.7 ns. Frames are paced to 30 a second (`DOOM_FPS_CAP`; the benchmark
runs unpaced).

At 250 MHz DOOM draws an E1M5 frame in about 18 ms on average, and about
33 ms in the heaviest scenes. Sending a frame to the LCD takes about 25 ms
at 41.6 MHz SPI. A second 64 KB screen to send from does not fit in RAM: the
zone already has only an 8 KB margin on the tightest map. So when DOOM waits
for the LCD, the two costs add up:

| E1M5 timedemo (`picolink.py bench`) | fps |
|---|---:|
| 150 MHz, DOOM waits for the LCD (before) | 23.7 |
| 250 MHz, DOOM waits for the LCD | 26.5 |
| 250 MHz, DOOM never waits for the LCD (`DOOM_LCD_TEAR`, default) | **36.0** |

With `DOOM_LCD_TEAR` (the default), a frame goes to the LCD whenever core 1
is idle and is sent while DOOM draws the next one into the same screen. The
LCD can show a torn frame. The USB video, DoomV and the lock-step hashes
(taken before present) are unaffected. `-DDOOM_LCD_TEAR=OFF` brings back
the tear-free wait.

300 MHz (clk_sys/3 flash at 100 MHz, 1.30 V) hung before USB came up on
this board, and needed BOOTSEL to recover.

## Where to go next

- **Tear-free at 30 fps.** A faster LCD clock would shorten the wait.
  ILI9341 modules usually manage 62.5 MHz on short wires, which would make
  the wait about 16 ms. This needs the panel wired to check.
- **Hot code in RAM.** The column and span drawers run from flash through the
  16 KB XIP cache, which the WAD's textures also stream through.
