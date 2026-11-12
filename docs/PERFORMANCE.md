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
*(To be confirmed on hardware.)*

## Where to go next

- **Overlap the LCD with the next frame.** The CPU waits for the SPI. A second
  64 KB frame buffer would let core 1 send one frame while core 0 draws the
  next -- but 64 KB is a quarter of the heap; it would cost E1M5.
- **Faster SPI.** `-DDOOM_SYS_MHZ=200` (or 250) overclocks the chip and its
  SPI with it; ILI9341 modules usually manage 50-62 MHz on short wires.
- **Hot code in RAM.** The column and span drawers run from flash through the
  16 KB XIP cache, which the WAD's textures also stream through.
