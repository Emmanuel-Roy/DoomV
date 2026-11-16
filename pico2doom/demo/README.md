# Demos

Scripts that show lock-step between a real Raspberry Pi Pico 2 and DoomV.
Run them from `pico2doom/` with the Pico plugged in over USB. Build the
firmware first: `cmake --build build/firmware`. That also builds the DoomV
guest the scripts replay into. Everything they write goes to `build/demo/`.

All of them show DoomV in its window. When a replay ends, or stops at a
divergence, the script prints its result and DoomV holds that frame until
you close its window. `histogram.py` is the exception: it closes DoomV
itself once it has counted.

`play.py` and `divergence.py` replay at the speed the Pico played (DoomV's
`-picolink-pace`), so what you see is in time with the recording.
`record_replay.py` and `histogram.py` replay as fast as DoomV can (about
100 fps), to show its speed.

| Script | What it shows | Pico needed |
|---|---|---|
| `python demo/corun.py` | **Co-simulation.** The Pico and DoomV run DOOM together, live. You play in the Pico's window; DoomV checks every frame as it arrives, and the Pico is never more than 2 frames ahead | yes |
| `python demo/record_replay.py` | **Record, then replay.** Records the Pico for 30 s, then replays the recording in DoomV and checks every frame | yes (`--replay FILE` to skip recording) |
| `python demo/divergence.py` | **A divergence.** Changes one thing in a copy of a recording and replays it: DoomV runs in step up to the change and stops on that frame, saying what differs | no (uses the newest recording) |
| `python demo/play.py [FILE...]` | **Watch a recording.** Plays one or more recordings in DoomV's window, still checking every frame; default the newest | no |
| `python demo/histogram.py` | **What DOOM executes.** Replays a recording counting every retired instruction by address, then reports the most executed instructions, instruction classes, the hottest functions, and DOOM vs port vs C runtime | no (uses the newest recording) |

## A run-through

```
python demo/corun.py --seconds 60        # play for a minute; both windows open
python demo/play.py                      # watch it again in DoomV
python demo/divergence.py --all          # four kinds of fault, each caught
python demo/histogram.py                 # then see build/demo/histogram/report.html
```

Measured (E1M1, 2,021 frames, real Pico 2 at 250 MHz):

- **Co-run:** every frame identical, 3.35 billion retired instructions of
  DOOM the same on the chip and in DoomV, 27 fps live. With the LCD at
  31.25 MHz, core 1 is busy sending to the display and also runs the USB
  link that paces the two; it reached 30 fps at 41.6 MHz.
- **Divergence**, fault at frame 1200: `hash`, `instret` and `clock` are all
  caught in frame 1200; `key` is caught at 1219, the first key press from
  1200 on. [docs/DIVERGENCE.md](../docs/DIVERGENCE.md) explains each.
- **Histogram:** 4.73 billion instructions, 91 distinct, 41% of them
  compressed. Loads are 27% and ALU work 48%. `R_DrawSpan` and
  `R_DrawColumn` (floors and walls) are a third of the run.

## Options

- `corun.py`: `--seconds N` (0: until Ctrl+C), `--warp E M`, `--headless`
  (DoomV without its window), `--ahead N`.
- `record_replay.py`: `--seconds N`, `--warp E M`, `--title` (the title
  screen and DOOM's own demos instead of a map), `--replay FILE`, `--headless`
  (DoomV without its window, about 80 fps).
- `play.py [FILE...]`: any number of files or patterns
  (`build/demo/*.p2dl`), played one after another.
- `divergence.py [FILE]`: `--kind hash|instret|clock|key`, `--all`,
  `--frame N` (default: the middle frame), `--record SECONDS`. `key` needs a
  recording with keyboard input: play in the Pico's window while recording.
- `histogram.py [FILE]`: `--frames N` (only the first N), `--record SECONDS`.
  Writes `report.html`, `instructions.csv` and `functions.csv`. It uses
  DoomV's `-histogram=FILE` option, which runs DoomV's exact
  one-step-at-a-time path and writes `pc count` lines. The replay is still
  checked against the Pico frame by frame. The histogram includes DOOM's
  start-up and the replay's own platform code (`backend_present` copies each
  frame to DoomV's screen; `port_hash_bytes` fingerprints it).
