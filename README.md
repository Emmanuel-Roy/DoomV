# Pico2Doom

**DOOM (shareware) on a Raspberry Pi Pico 2**, running on the RP2350's
**RISC-V** cores, with a $10 SPI display and a handful of buttons -- and
checked, frame by frame, against my RISC-V emulator
[DoomV](https://github.com/Emmanuel-Roy/DoomV).

- **520 KB of RAM.** Stock DOOM needs ~920 KB for its biggest map. Nine small,
  verified engine patches cut that down to the 5 maps that fit.
- **Unchanged game.** All three shareware demos play with the identical game state on every frame
  and identical pictures except one pixel, compared with stock doomgeneric.
- **Measured.** Live per-phase timing on screen and over USB, plus DOOM's own
  `timedemo` benchmark.
- **Lock-step with DoomV.** The Pico records its inputs, picture hashes and
  retired-instruction counts; DoomV replays them on the same machine code
  and halts at the first difference.

## Hardware

| Part | |
|---|---|
| Raspberry Pi Pico 2 (RP2350) | core 0 runs DOOM; core 1 does all the I/O -- display, buttons, USB |
| ILI9341 2.4"/2.8" SPI display, 320×240 | DOOM in the top 200 rows, performance read-out below |
| 8 push buttons | between a GPIO and GND -- no resistors needed |

### Wiring

| Display pin | Pico 2 pin |  | Button | Pico 2 pin |
|---|---|---|---|---|
| VCC | 3V3 (pin 36) | | Up | GP2 |
| GND | GND | | Down | GP3 |
| CS | GP17 | | Left | GP4 |
| RESET | GP21 | | Right | GP5 |
| DC / RS | GP20 | | Fire (and *select* in menus) | GP6 |
| SDI / MOSI | GP19 | | Use (and *yes*) | GP7 |
| SCK | GP18 | | Menu (Esc) | GP8 |
| LED | GP22 (or 3V3) | | Weapon: tap = next weapon, hold + Left/Right = strafe, hold + Up = map | GP9 |

Pins are in [`firmware/board_config.h`](firmware/board_config.h). If the
picture is mirrored or upside down, change `LCD_MADCTL` there.

## Build and flash

You need: CMake, Ninja, Python 3, the
[xPack RISC-V GCC](https://xpack-dev-tools.github.io/riscv-none-elf-gcc-xpack/)
(`riscv-none-elf-gcc`, the compiler DoomV uses), a host C++ compiler (for
picotool), and the shareware `DOOM1.WAD` in [`wad/`](wad/README.md).

```sh
git clone --recursive <this repository>     # or: git submodule update --init --recursive
cmake -S firmware -B build/firmware -G Ninja
cmake --build build/firmware
```

Hold **BOOTSEL** while plugging the Pico in, and copy
`build/firmware/pico2doom.uf2` to the drive that appears. After the first
time, `python tools/picolink.py flash` does it without the button.

## Controls

D-pad moves and turns, **Fire** shoots, **Use** opens doors, **Menu** opens
the menu (Fire selects). Always-run is on.

Or play from the PC -- no buttons needed: `python tools/picolink.py play`
shows the Pico's screen in a window (over USB) and sends the keyboard to it
(arrows, Ctrl fire, Space use, Esc menu, Enter select, 1-7 weapons). PC keys
and button presses go into the same input buffer, and every key DOOM reads,
from either, is reported to the PC -- and recorded for DoomV. Maps that fit: **E1M1, E1M4,
E1M5, E1M8, E1M9** -- the others show a "not enough RAM" screen.

## Performance

The bottom of the screen shows, once a second: frames per second, the frame
time, and where it went -- game logic, walls, floors, sprites, status bar,
and sending the frame to the LCD -- plus heap use. The same line goes to the
Pico's USB console:

```sh
python tools/picolink.py console --csv perf.csv   # live, and logged to a table
python tools/picolink.py bench                    # DOOM's timedemo on DEMO1
```

Hold **Fire** while powering on to run the benchmark without a PC. Numbers
and analysis: [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## How it works

| Folder | |
|---|---|
| [`external/`](external) | submodules: doomgeneric (pinned to DoomV's commit), pico-sdk 2.2.0, picotool |
| [`port/`](port) | the platform layer shared by every target: video, WAD, wipe, lock-step, profiler |
| [`patches/`](patches/README.md) | the engine patches, applied to a copy at build time |
| [`firmware/`](firmware) | the Pico 2: display, buttons, USB |
| [`doomv/`](doomv) | the same DOOM as a DoomV guest, for lock-step |
| [`sim/`](sim) | a PC build with the Pico's memory limits, for measuring and testing |
| [`tools/`](tools) | WAD packer, memory budget, `picolink.py`, ISA comparison, RV32 check |

- [docs/MEMORY.md](docs/MEMORY.md) -- 4 MB of flash, 520 KB of RAM: what went where
- [docs/PERFORMANCE.md](docs/PERFORMANCE.md) -- measurements and where the time goes
- [docs/LOCKSTEP.md](docs/LOCKSTEP.md) -- holding DoomV to real hardware, at retirement

DOOM is © id Software. doomgeneric is GPL-2.0 ([its licence](external/doomgeneric/LICENSE)),
and so is this port, which builds on it.
