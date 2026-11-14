# Why a divergence happens, and why it is caught where it is

Lock-step ([LOCKSTEP.md](LOCKSTEP.md)) says the Pico 2 and DoomV must do
exactly the same thing. This page is about the moment they don't: what can
make them split, where DoomV notices, and how to read what it reports.
`demo/divergence.py` makes each kind happen on demand.

## The contract

DOOM is deterministic. Two machines agree as long as three things hold:

1. **Same code.** The firmware and the DoomV guest are the same machine code
   wherever DOOM runs (checked on every build, see
   [LOCKSTEP.md](LOCKSTEP.md#identical-builds)).
2. **Same inputs, in the same order.** DOOM has exactly two inputs: the
   clock (`DG_GetTicksMs`) and the keys (`DG_GetKey`). The Pico records every
   value they return, and DoomV hands back exactly those values.
3. **Same execution.** Every instruction does what the RISC-V specification
   says, on the chip and in the emulator.

A divergence is one of these breaking. Something differs, and from then on
the machines compute different things.

## Where DoomV looks

DoomV checks at two kinds of point, and a divergence is reported at the
first one the difference reaches:

| Checkpoint | When | What must match |
|---|---|---|
| **Every input read** | each time DOOM asks for the clock or a key | the kind of input: DOOM must ask for a clock reading exactly where the Pico's DOOM did, and for a key where it did |
| **Every frame end** | ~1.7 million instructions apart | picture hash, game-state hash, retired instructions in game logic, retired instructions in rendering |

So a difference is caught on the frame it first shows up in. It may be
caught mid-frame if it changes which input DOOM asks for next. It is never
caught later than the end of the frame where it first changes something
checked.

## The four faults in the demo

`demo/divergence.py` copies a real recording and changes one thing in the
copy, at a frame you choose. Changing the Pico's side of the record has the
same effect as the Pico itself having done it, and it can be repeated on
stage. Measured on a 2,021-frame E1M1 recording from the real Pico,
fault at frame 1,200:

| `--kind` | What was changed | Caught | What DoomV reported |
|---|---|---|---|
| `hash` | one bit of frame 1200's picture hash | frame 1200 | picture hash only |
| `instret` | frame 1200's rendering count +1 | frame 1200 | rendering instructions only (-1) |
| `clock` | one clock reading in frame 1200, +100 ms | frame 1200, mid-frame | "DOOM read a key, but the Pico's next input was a clock reading" |
| `key` | the first key press from frame 1200 on (at frame 1219), *right* -> *up* | frame 1219 | all four: picture, game state, game logic +61, rendering +10,737 |

In every case the 1,199 (or 1,218) frames before matched exactly, about
2 billion retired instructions. DoomV stops on the faulty frame itself.

### hash: one wrong result

The picture hash is one of the values compared, so the frame it is on fails
at once. Nothing DoomV computed is wrong: the game state and both
instruction counts match, and only the picture's fingerprint disagrees. This
is what one wrong result on the chip looks like, for example a flipped bit
in a pixel. It is also how an emulator bug in a drawing instruction showed
up (see *Real divergences* below).

### instret: one instruction more

The Pico says it retired one more instruction while rendering frame 1200.
The picture and the game state are identical, yet the frame fails. Comparing
at retirement catches work that leaves no visible trace: an extra
instruction, a different path through `memset`, a loop that ran once more
and computed the same answer.

### clock: an input delivered wrong changes the control flow

DOOM runs one game tic per 1/35 s it sees pass. Each frame it reads the clock
and runs every tic that is due. The Pico's DOOM read the clock, found no new
tic due yet, and read it again. DoomV's DOOM got a reading 100 ms later, so
3 tics were due at once, and it went to run them. A tic starts by reading
the keys, so its next input request was a key where the Pico's was a clock
reading. The sequence of inputs split, so DoomV stopped mid-frame, before
any picture was drawn. A wrong clock is caught at the next input request,
not at the next frame end.

### key: an input delivered wrong changes the game

DoomV got *up* where the Pico read *right*. DOOM reads the keys at the
start of each tic and builds that tic's move from them. So in the very frame
the key arrives, the player moves differently, and everything differs at
once: the picture, the game state and both instruction counts. Game logic
ran 61 more instructions, and the renderer drew a different view
(10,737 more). The key changed was the first press from frame 1200 on. It
was at frame 1219, which is why the fault and the catch are both at 1219.

## Real divergences

Lock-step against real recordings has found these. Each one matches a kind
above:

| What was wrong | How it showed | Kind |
|---|---|---|
| DoomV computed `zext.h`, `rol`, `clz`, `rev8`, `pack`... with 64-bit semantics in 32-bit mode (1,641 of 15,360 results wrong) | frame 38: picture hash differs, game state the same. `zext.h` is how GCC widens every `unsigned short`, so a few hundred texels per frame were wrong | `hash` |
| the two builds were not the same code: pico-sdk wrapped `malloc` and `printf` (also inside newlib), and newlib's `memset`/`memmove` aligned to 16 bytes while data sits at different addresses mod 16 in the two images | one frame's retired count off by 64 at DEMO1's start, with picture and state identical | `instret` |
| a stale DoomV guest built from older code | diverged at frames 57, 148 and 160 | any: different code computes different things |
| DoomV did not truncate RV32 addresses to 32 bits | the guest faulted at the first access above `0x80000000` | (before the first frame) |

All are fixed. The `hash`-like case is the one per-frame lock-step is built
for: an instruction computed wrong, caught within a frame, without a
trace port.

## Reading a report

Which fields differ narrows down the cause:

| What DoomV reports | Likely cause |
|---|---|
| the picture only | a rendering computation differs, but the game is the same: a drawing instruction, the palette or the screen |
| the game state (with or without the picture) | game logic computed differently, or an input was delivered wrong |
| retired counts only | the same results by a different path: a build difference, an alignment-dependent loop, a counted window holding code the other side doesn't run |
| "DOOM read a key, but the Pico's next input was a clock reading" (or the reverse) | control flow that depends on an input went a different way: a clock value delivered wrong, or code that decides differently whether a tic is due |

## After the stop

When DoomV stops it leaves, in its working directory:

- `crash.log`: the whole machine state at the stop (registers, CSRs, pc).
- `picolink_doomv.ppm`: DOOM's picture as DoomV drew it, to compare with
  the Pico's frame. `demo/divergence.py` also saves it as `doomv_frame.png`.

From there:

- `PICOLINK_HASHES_ONLY=1` keeps going past retired-count differences and
  stops only when the picture or game state differs. Use it to see whether
  a count difference ever reaches the game.
- `-DPORT_FUNCPROF=ON` charges every retired instruction to a function on
  both machines, and `tools/funcprof_diff.py` names the functions whose
  counts differ ([LOCKSTEP.md](LOCKSTEP.md#drilling-down)).
- DoomV's own instruction-by-instruction lock-step against Sail, the
  RISC-V reference model, can start from the stopped state.
- `demo/histogram.py` shows which instructions and functions a stretch of
  the run executes.
