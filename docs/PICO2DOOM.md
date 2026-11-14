# Pico2Doom: DOOM on a Pico 2, checked by DoomV

[`pico2doom/`](../pico2doom/README.md) runs DOOM on a Raspberry Pi Pico 2. The
Pico 2 uses an RP2350 chip, and the port runs on its Hazard3 RISC-V cores. A
second build of the same code runs as a DoomV guest. A Python bridge on the PC
connects the two. The Pico records everything DOOM reads, and DoomV replays the
recording with the same machine code. Each frame is compared at retirement:
two hashes and two counts of retired instructions.

This guide explains how the firmware works, how the Python bridge works, and
how DoomV checks what arrives. The details of the comparison itself are in
[LOCKSTEP.md](../pico2doom/docs/LOCKSTEP.md). What happens when the two
machines disagree is in [DIVERGENCE.md](../pico2doom/docs/DIVERGENCE.md).

```
 Pico 2 (RP2350)                       PC
 ┌──────────────────────────┐   USB    ┌───────────────────────────┐
 │ core 0: DOOM only        │  3 CDC   │ tools/picolink.py         │
 │   port/ (shared code)    │  ports   │   saves the link stream   │
 │   records clock + keys,  │ ───────► │   to a .p2dl file,        │
 │   hashes every frame     │          │   shows the video,        │
 │ core 1: all I/O          │ ◄─────── │   sends keys and pacing   │
 │   USB, LCD, buttons      │ commands └─────────────┬─────────────┘
 └──────────────────────────┘                        │ the file (growing, if live)
                                       ┌─────────────▼─────────────┐
                                       │ DoomV -picolink=FILE      │
                                       │   guest: the same code    │
                                       │   (pico2doom/doomv/)      │
                                       │   halts at the first      │
                                       │   difference              │
                                       └───────────────────────────┘
```

## Where the code is

| Directory | What it holds |
|---|---|
| [`pico2doom/port/`](../pico2doom/port/port.h) | The code shared by every target. It holds doomgeneric's `DG_*` layer, the recorder, the hashes, the retired-instruction windows, video, the WAD and the C library glue. |
| [`pico2doom/firmware/`](../pico2doom/firmware/main.c) | The Pico 2 backend. It covers boot, core 1's I/O loop, USB, the ILI9341 display and the buttons. |
| [`pico2doom/doomv/`](../pico2doom/doomv/doomv_main.c) | The DoomV backend. It is the guest that replays a recording. |
| [`pico2doom/sim/`](../pico2doom/sim/README.md) | A PC build that keeps the Pico's memory limits. It is used to measure memory. |
| [`pico2doom/patches/`](../pico2doom/patches/README.md) | Small changes to the engine, mostly so that it fits in 520 KB. |
| [`pico2doom/tools/picolink.py`](../pico2doom/tools/picolink.py) | The PC side of the USB link. |
| [`src/pico_link.cpp`](../src/pico_link.cpp) | DoomV's replay device, attached with `-picolink=FILE`. |
| [`pico2doom/demo/`](../pico2doom/demo/README.md) | The presentation demos, built on the pieces above. |

## The seam: one port layer, three backends

doomgeneric reduces a platform to a handful of `DG_*` functions. Pico2Doom
implements those functions once, in `port/`, and gives each target a smaller
*backend* below them ([`port/port.h`](../pico2doom/port/port.h)). A backend
supplies these things:

- the clock (`backend_ticks_ms`), the keys (`backend_get_key`) and a cycle
  counter for profiling;
- a way to show a finished 320×200 8-bit frame (`backend_present`);
- the WAD (`backend_wad`) and the memory DOOM's zone may use (`backend_zone`);
- the lock-step hooks: a mode (none, record or replay), a byte sink for
  recording, replay sources for the clock and keys, and a per-frame check.

Everything above the backend is the same source code. The firmware build also
builds the DoomV guest and fails unless the two are the same machine code
wherever DOOM is counted (see *Identical builds* below). That shared code is
what makes a replay meaningful.

### DOOM's only two inputs

DOOM is deterministic. With the same WAD, the same clock readings and the same
key events in the same order, it computes the same frames. Both inputs reach
the engine through `DG_GetTicksMs` and `DG_GetKey`, in
[`port/port_link.c`](../pico2doom/port/port_link.c).

- **Recording (the Pico):** each value returned is also written as a record.
  `TICKS` carries a new clock value. `TICKS_REP n` means "the same value
  again, n more times". DOOM spins on the clock while it waits for the next
  tic, so most readings repeat, and a repeat costs one byte. `KEY` carries a
  key event, and `NOKEY` means "no event".
- **Replaying (DoomV):** the same two functions read the values from DoomV's
  link registers instead.

The bodies of both functions are platform code, so the instructions they
retire are measured and subtracted from the counted windows
(`port_link_excluded_instret`).

### At the end of every frame

`I_FinishUpdate` ([`port/port_video.c`](../pico2doom/port/port_video.c))
presents the frame and then calls `port_link_frame`, which computes four values:

| Value | How |
|---|---|
| frame hash | FNV-1a over the 64,000-byte 8-bit screen |
| game-state hash | `gametic`, `gamestate`, `leveltime`, both RNG indices, and on a level every mobj's type, position, momentum, angle, health, tics, state and flags, every sector's floor, ceiling and light, and the player's health, armour, weapon and view height. It never hashes a pointer, because the two machines place the zone at different addresses. |
| retired instructions, game logic | `minstret` around `G_Ticker` ([`port/port_perf.c`](../pico2doom/port/port_perf.c)) |
| retired instructions, rendering | `minstret` around BSP, planes, sprites and the status bar |

The Pico writes these values as a `FRAME` record. DoomV's guest writes them to
the link device, which compares them with the Pico's record. Retired
instructions are compared because only they are architectural. The Hazard3
pipeline fetches, squashes and stalls on instructions that never complete, and
none of that can be held against an emulator that executes one instruction at
a time.

### Identical builds

Every `cmake --build build/firmware` also runs these steps:

1. It builds `doomv/` with the same toolchain and flags
   (`rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb`, Hazard3's ISA). The output
   goes in `build/firmware/doomv-guest/`.
2. It runs [`tools/compare_isa.py`](../pico2doom/tools/compare_isa.py) on both
   ELFs. The script fails the build unless every function reachable from a
   counted window is the same code in both: about 422 functions and 51,700
   instructions, down through newlib and libgcc.

Making that true meant several changes:

- The Pico no longer uses pico-sdk's `malloc` and `printf` wrappers.
- Both builds share one C library layer
  ([`port/port_libc.c`](../pico2doom/port/port_libc.c)).
- `memset` and `memmove` look only at alignment mod 4.
- The zone is zeroed on both machines.
- The flags are pinned on every shared file.

[LOCKSTEP.md](../pico2doom/docs/LOCKSTEP.md#identical-builds) has the full
table.

### Starting DOOM the same way

`port_run` ([`port/port_main.c`](../pico2doom/port/port_main.c)) does these
things on every target, before DOOM starts:

1. It takes the zone from the backend and zeroes it.
2. When recording, it opens the recording with a `HELLO` record (magic
   `P2DL`, version, WAD size, zone size) and an `ARGS` record (DOOM's extra
   arguments, such as `-warp 1 1`).
3. It runs a `minstret` self-test whose results go into the recording.

DoomV's guest reads the zone size and the arguments back from the link
device. Its DOOM therefore gets exactly the Pico's heap and command line. That
matters because allocations decide which cache blocks are purged, and whether
the screen melt fits.

## The firmware

### Boot ([`firmware/main.c`](../pico2doom/firmware/main.c))

1. **Clocks.** The default overclock is 250 MHz (`DOOM_SYS_MHZ`). Boot raises
   the core voltage and retimes the flash for the higher clock: the divider
   keeps the flash at or under 100 MHz, and the sample delay stays near
   6.7 ns. That routine runs from RAM. It then moves `clk_peri`, which drives
   the SPI, onto the system clock. Left on the 48 MHz USB PLL, `clk_peri`
   would cap the display's SPI at 12 MHz.
2. **Mode.** The PC selects the next boot's mode by writing a magic value to
   a watchdog scratch register, which survives a reboot. Holding FIRE at
   power-on selects the benchmark.

   | Mode | What runs |
   |---|---|
   | play | DOOM, paced to 30 fps |
   | benchmark | DOOM's own `-timedemo demo1`, unpaced, with the result on screen |
   | link | the run is recorded over USB for DoomV. The mode waits for `picolink.py` to hold the port open for 250 ms, so the recording starts at DOOM's very first clock read. A second scratch register can carry `-warp E M`. |

3. **Display, then core 1.** `lcd_init` sets up the ILI9341. The setup
   commands go at 10 MHz or less, then pixel data at the fastest even divider
   of `clk_peri` that is 32 MHz or less (31.25 MHz at 250 MHz). After that the
   display belongs to core 1, which `io_start` launches.
4. **Link mode only:** core 0 disables its interrupts for good, then calls
   `port_run`. An interrupt handler would retire instructions inside a counted
   window, and the counts would no longer match DoomV's. Core 0 needs no
   interrupts: USB and the display belong to core 1, the clock is read
   directly, and `backend_sleep_ms` busy-waits on the timer instead of
   sleeping on an interrupt.

### Two cores, one job each

**Core 0 runs DOOM and nothing else.** It never waits on a device, apart from
the hand-offs described below, and in link mode it takes no interrupts. Its
stack is the 8 KB in SCRATCH_X and SCRATCH_Y. That stack is painted at boot so
the high-water mark can be reported.

**Core 1 does all the I/O** ([`firmware/core1_io.c`](../pico2doom/firmware/core1_io.c)).
It runs one loop, forever:

```
for (;;) {
    usb_service(host_byte);     // TinyUSB task, drain the rings, read PC commands
    buttons_poll(button_event); // debounce, turn presses into DOOM key events
    if a frame job is posted:   // convert and send the next 2-line strip to the LCD,
        lcd_frame_step();       // and the matching rows of PC video
    if a message job is posted:
        lcd_message();          // full-screen text (errors, benchmark result)
}
```

Each piece of data that crosses between the cores has exactly one writer and
one reader. A memory fence on each side is therefore all the synchronisation
needed, and there are no locks:

| Hand-off | Writer → reader | What |
|---|---|---|
| input buffer (64 events) | core 1 → core 0 | key events from the buttons *and* from the PC, in arrival order. `DG_GetKey` drains it, so DoomV sees PC and button input alike. |
| console ring (1 KB) | core 0 → core 1 | log lines. If no terminal is listening, or the ring is full, they are dropped. |
| link ring (2 KB) | core 0 → core 1 | the recording. It is **never dropped**: core 0 waits for room instead. Short DTR drops, which happen when a host opens the port, are waited out. Only a host gone for 2 s counts as gone. |
| display job | core 0 → core 1 | a pointer to the frame, the back-buffer tail and the palette. Core 1 publishes how many rows it has read. |
| HUD line | core 0 → core 1 | the once-a-second performance line drawn under the picture |
| pacing (`G`) and halt (`H`) | PC via core 1 → core 0 | the co-run controls, described below |

### Getting a frame to the display without a second screen

DOOM draws into a single 64 KB 8-bit screen, and there is no RAM for a second
one. A full RGB565 copy would take 128 KB, which is a quarter of the chip.

- **Converting in strips.** Core 1 converts two lines at a time into one of
  two 1,280-byte strip buffers, while DMA streams the other strip to the SPI
  FIFO ([`firmware/lcd_ili9341.c`](../pico2doom/firmware/lcd_ili9341.c)).
  Converting a strip takes about 15 µs and sending it about 270 µs, so the CPU
  is never the bottleneck.
- **Borrowing a back buffer.** `I_FinishUpdate` tries to borrow a copy of the
  frame's bottom rows from the zone as a *cache* block. It tries 200 rows,
  then 100, then 50. The zone takes the block back the moment DOOM needs the
  room. Core 0 waits only until core 1 has read the rows that are *not*
  copied, then draws the next frame while core 1 sends the copied tail. The
  DoomV guest runs the same decision, so its zone stays identical.
- **Tear mode (`DOOM_LCD_TEAR`, on by default).** Core 0 never waits for the
  display at all. A frame is handed over only when core 1 is idle, and is sent
  while DOOM draws the next one into the same screen. The LCD can then show a
  torn frame. The hashes are taken from DOOM's screen, not the LCD, so tearing
  does not affect them.
- **Pacing.** `backend_present` busy-waits for a fixed 1/30 s slot
  (`DOOM_FPS_CAP`), giving a locked frame rate. The wait is outside every
  counted window. The clock values DOOM reads afterwards are recorded like any
  others, so pacing changes nothing DoomV compares.

At 31.25 MHz a frame takes core 1 about 36 ms to send. Core 1 also runs USB,
so a live co-run reaches 27 fps; at 41.6 MHz it reached 30.
[PERFORMANCE.md](../pico2doom/docs/PERFORMANCE.md) has the measurements.

### USB: three serial ports

The firmware is a composite USB device with VID:PID `2E8A:4D44`
([`firmware/usb_descriptors.c`](../pico2doom/firmware/usb_descriptors.c)). It
presents three CDC serial ports:

| Interface | Port | Direction | Contents |
|---|---|---|---|
| 0 | console | Pico → PC | plain text: the banner, the performance line once a second, errors |
| 2 | link | both | Pico → PC: the recording ([`port/port_link.h`](../pico2doom/port/port_link.h)). PC → Pico: one-letter commands. |
| 4 | video | Pico → PC | a half-size copy of every Nth frame, sent by core 1 |

The commands the PC can send on the link port (`host_byte` in `core1_io.c`):

| Command | Bytes | Effect |
|---|---|---|
| `K` | pressed, key | push a key event into the input buffer, exactly as a button would |
| `V` | n | stream every nth frame on the video port (0 stops it) |
| `R` | — | reboot into link mode |
| `W` | e, m | reboot into link mode, starting DOOM with `-warp e m` |
| `B` | — | reboot into the benchmark |
| `U` | — | reboot into BOOTSEL, ready for a new UF2 |
| `G` | u32 frame | co-run pacing: DOOM may finish frames up to this one, then waits |
| `H` | u32 frame | co-run: DoomV diverged at this frame, so stop |

**Video** goes out on its own port so that it can never slow DOOM down. For
each frame sent, core 1 writes a header and then one record per row:

- the header is `0x5A`, the frame number (u32) and the palette as 256 RGB565
  words;
- each row record is `0xA5`, the row number (0–99), and 160 pixels taken from
  the even rows at half width.

A row that does not fit in the USB FIFO at that moment is dropped, and the PC
keeps that row's last picture. Video used to go out on the link port from
core 0, where 64 KB a frame at USB full speed held DOOM up for about 60 ms.

When DOOM stops (`I_Error`, quitting from the menu, or a divergence in a
co-run), `backend_fatal` shows the reason on the LCD and also sends it as a
`TEXT` record. A button press or a key from the PC then reboots the board.
Core 1 keeps running throughout, so the PC can still reflash it.

## The recording format (`.p2dl`)

A recording is the link port's byte stream, saved exactly as it arrived. All
values are little-endian, and every record starts with a tag byte
([`port/port_link.h`](../pico2doom/port/port_link.h)):

| Tag | Name | Body | Meaning |
|---|---|---|---|
| 0x06 | HELLO | magic `P2DL`, version 2, WAD size, zone size (4×u32) | always first |
| 0x0A | ARGS | u8 length, text | DOOM's extra arguments, always right after HELLO |
| 0x01 | TICKS | u32 | `DG_GetTicksMs` returned this |
| 0x02 | TICKS_REP | u8 n | it returned the previous value n more times |
| 0x03 | KEY | u8 pressed, u8 key | `DG_GetKey` returned this event |
| 0x04 | NOKEY | — | `DG_GetKey` had nothing |
| 0x05 | FRAME | frame, frame hash, state hash, instret logic, instret render (5×u32) | the end of a frame |
| 0x0B | INPUT | u8 source (0 buttons, 1 PC), pressed, key | where a key came from, for the PC to display. Informational only. |
| 0x09 | TEXT | u8 length, text | a log line, such as the self-test results or the reason DOOM stopped |
| 0x07, 0x08 | VIDEO, PALETTE | a full frame or palette | from older firmware. The PC shows them and DoomV skips them. |

The inputs for frame *k* are the records between FRAME *k*−1 and FRAME *k*. A
30-second E1M1 run is about 27 KB.

## The Python bridge: `tools/picolink.py`

[`picolink.py`](../pico2doom/tools/picolink.py) is the PC end of the USB link.
It uses only `pyserial` and `tkinter`. These are its commands:

| Command | What it does |
|---|---|
| `console [--csv FILE]` | prints the console port. `--csv` also logs the performance line as a table. |
| `bench` | sends `B`, waits for the board to return, and prints DOOM's `timed N gametics in M realtics` result |
| `play [--video N]` | opens a window with the Pico's screen and sends the PC keyboard to the Pico |
| `record FILE [--warp E M] [--seconds S] [--video N]` | reboots into link mode and saves the link stream to FILE |
| `corun [FILE] [--ahead N] [--headless]` | records the Pico while DoomV replays the recording live, as described below |
| `flash [UF2]` | sends `U`, waits for the RP2350 BOOTSEL drive and copies the UF2 onto it |

### Finding the board

`_our_ports()` lists the serial ports with VID:PID `2E8A:4D44` and assigns
roles by USB interface number:

- the port on interface 2 is the link;
- the port on interface 4 is the video;
- the remaining one is the console.

The interface number is read from the port's location or hardware ID
(`x.2`, `:1.2` or `MI_02`, depending on the OS). Windows does not always
report a location for interface 0, which is why the console is found by
elimination. `open_port` retries for a few seconds, because just after a
reboot Windows can list a port before it can be opened. It then sets DTR,
which is what TinyUSB reports as "connected".

A reboot (`reboot_into`) sends a one-byte command on the link port, waits for
the board to drop off the bus and come back, and finds its ports again.

### A session: `run_session`

`record`, `play` and `corun` share one function, which runs these threads:

- **The link reader** reads whatever has arrived on the link port. It writes
  all of it to the output file, unparsed, and flushes after every read. That
  matters because DoomV may be reading the same file live. The reader also
  parses the stream to count frames, print `TEXT` and `INPUT` records, and feed
  older-firmware video to the window.
- **The video reader** reads the video port and assembles 160×100 frames from
  the `0x5A` header and `0xA5` row records. It converts the RGB565 palette back
  to RGB and hands each frame to the window.
- **The window** (`Viewer`) is tkinter, shown at 4× zoom. Key presses become
  `K pressed key` commands on the link port. The OS's auto-repeat is filtered
  out, because DOOM wants one press, not a stream of them. Keys go into the
  same input buffer as the buttons, so they are recorded like any other input.
- **The main thread** runs the window, or a status line when there is no
  window, until `--seconds` runs out, the window is closed or Ctrl+C is
  pressed. It then sends `V 0` to stop the video and closes the file.

### Co-running: `corun`

`corun` keeps the Pico and DoomV in step while the game is being played:

1. It reboots the Pico into link mode (`W e m` or `R`).
2. As soon as the link port is open, and before DOOM's first frame, it sends
   `G <ahead>`. From then on the Pico may finish only `ahead` frames (2 by
   default) before it waits.
3. It starts DoomV on the file being written:

   ```
   riscv_doom doom1.wad pico2doom-doomv.elf -march=rv32imac_zicsr_zifencei_zba_zbb_zbs_zbkb -picolink=FILE
   ```

   It runs DoomV with `PICOLINK_ACK=1`, which makes DoomV print
   `picolink-ack N` after each frame that matched. When the file runs dry,
   DoomV's link device waits for more bytes rather than treating it as the end.
4. A thread reads DoomV's output. For each ack it sends `G <N + ahead>`, which
   lets the Pico run on. On `DIVERGED` it sends `H <frame>`. The Pico stops
   after its frame record, shows "DoomV diverged at frame N", and both
   machines are left on the same frame. DoomV stays halted there for
   inspection.
5. Another thread reads the Pico's console for its frame rate, which goes into
   the window title next to the frame counts.

Where the Pico waits matters: in `backend_link_check`, after the frame's
record has gone out and outside every counted window. The clock values DOOM
reads after the wait are recorded like any others. Holding the Pico back
therefore changes nothing DoomV compares; it only means DoomV is never more
than a couple of frames behind.

## DoomV's side

### The guest ([`pico2doom/doomv/doomv_main.c`](../pico2doom/doomv/doomv_main.c))

The guest is the same port layer with a DoomV backend:

- **Mode.** At start-up it reads the link's zone register. A non-zero value
  means a recording is attached, which selects replay mode.
- **Start-up.** It reads the Pico's DOOM arguments a byte at a time, and takes
  a zone of exactly the recorded size from a 1 MB static buffer.
- **Clock and keys.** These come from the link registers in replay mode.
  Without a recording, the guest uses DoomV's own timer and keyboard.
- **Display.** `backend_present` expands the 8-bit frame through the palette
  into DoomV's framebuffer, four pixels per word read.
- **Frame check.** `backend_link_check` writes the frame number, the two
  hashes and the two retired counts to the frame register. The fifth write
  triggers the comparison.

### The link device ([`src/pico_link.cpp`](../src/pico_link.cpp))

`-picolink=FILE` attaches a small MMIO device:

| Address | Access | Meaning |
|---|---|---|
| `0x1000001C` | read | the recorded zone size (0: no recording) |
| `0x10000020` | read | the next clock value |
| `0x10000024` | read | the next key event: 0, or `0x10000 \| pressed<<8 \| key` |
| `0x10000028` | write ×5 | frame, frame hash, state hash, instret logic, instret render |
| `0x1000002C` | read | the next byte of DOOM's arguments, 0 at the end |

Each read returns the next value from the recording, and it checks the guest's
I/O sequence along the way.

- **Input reads.** If the guest asks for the clock where the Pico read a key,
  or the reverse, that is a divergence. It is caught mid-frame, at the input
  request itself.
- **Frame writes.** The five values are compared with the Pico's `FRAME`
  record. If any differs, DoomV prints which values differ and by how much,
  and halts the machine.

When DoomV halts, it writes two files: `crash.log`, with the whole machine
state, and `picolink_doomv.ppm`, with DOOM's picture as DoomV drew it. From
the halt, DoomV's own tools can take over: lock-step against Sail, the
debugger, and `-histogram`.

Two environment variables change the device's behaviour:

- `PICOLINK_HASHES_ONLY=1` reports differences in the retired counts but
  stops only when a hash differs.
- `PICOLINK_ACK=1` prints the per-frame acks that `corun` paces with.

DoomV's `-histogram=FILE` option counts every retired instruction by address
during a replay. `pico2doom/demo/histogram.py` turns those counts into a
report.

## One frame, start to finish

The life of one frame in a co-run:

1. On the Pico, core 0 runs `TryRunTics`. It reads the clock, gets `TICKS` or
   `TICKS_REP` records, and for each tic that is due reads the keys and gets
   `KEY` or `NOKEY` records. These go into the link ring.
2. DOOM renders inside the counted windows. `I_FinishUpdate` hands the frame
   to core 1, then hashes it, and the `FRAME` record goes into the link ring.
3. `backend_link_check` waits there if DoomV is `ahead` frames behind.
4. Core 1 drains the ring into the link port, sends the picture to the LCD in
   strips, and sends half-size video rows on the video port.
5. On the PC, `picolink.py` appends the bytes to the `.p2dl` file and shows
   the video.
6. DoomV's guest reaches the same clock and key reads and gets the same values
   from the file. It renders the same frame and writes its five values.
7. The values match, so DoomV prints `picolink-ack N`. `picolink.py` sends
   `G N+ahead`, and the Pico runs on.

## Further reading

- [pico2doom/README.md](../pico2doom/README.md): building, flashing, wiring
  and playing.
- [LOCKSTEP.md](../pico2doom/docs/LOCKSTEP.md): what is compared and why, the
  identical-build checks, and drilling down into a difference.
- [DIVERGENCE.md](../pico2doom/docs/DIVERGENCE.md): why a divergence happens,
  where it is caught, and how to read the report.
- [MEMORY.md](../pico2doom/docs/MEMORY.md): fitting DOOM into 520 KB, and
  which maps fit.
- [PERFORMANCE.md](../pico2doom/docs/PERFORMANCE.md): frame times, the second
  core, the overclock and the LCD clock.
- [demo/README.md](../pico2doom/demo/README.md): the co-run, record-and-replay,
  play, divergence and histogram demos.
