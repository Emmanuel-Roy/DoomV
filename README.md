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
| Zba / Zbb / Zbs (bitmanip) | ✅ |
| Zcb (compressed bitmanip/mem) | ✅ |
| Zicond (conditional move) | ✅ |
| Zvbb / Zvbc (vector bitmanip, carry-less multiply) | ✅ |
| Zbc / Zbkb / Zbkx (scalar carry-less multiply, crypto bitmanip) | ✅ |
| Zkr (entropy source `seed`) | ✅ |
| Zvkned / Zvkg (vector AES, GCM/GMAC) | ✅ |
| Zvknha / Zvknhb (vector SHA-256 / SHA-512) | ✅ |
| Zvksed / Zvksh (vector SM4, SM3) | ✅ |
| Zicfilp / Zicfiss (landing pads, shadow stack) | ✅, off by default |
| Zihintpause / Zihintntl | ✅ (hints — retire without effect) |
| Zimop / Zcmop | ✅ (write zero to `rd`) |
| Zicbom / Zicbop | ✅ (no cache to manage — see below) |
| Zicboz (`cbo.zero`) | ✅ |
| Zawrs (`wrs.nto`/`wrs.sto`) | ✅ (retires immediately) |
| Zicntr (`cycle`/`time`/`instret`) | ✅ |
| Zihpm (`hpmcounter3-31`) | ✅ (read as zero) |
| Zfa (additional FP) | ✅ |
| Zfh / Zfhmin (half-precision arithmetic, converts) | ✅ |
| Zvfh / Zvfhmin (vector half arithmetic, converts) | ✅ |
| Zvfbfmin / Zvfbfwma (vector bf16 converts, widening FMA) | ✅ |
| Svinval (fine-grained TLB invalidation) | ✅ (no TLB — see below) |
| Svnapot (64KB contiguous PTEs) | ✅ |
| Svpbmt (page-based memory types) | ✅ |

The bitmanip families and `Zicond` are on by default despite Doom never
emitting them: every modern riscv64 Linux userspace assumes them, so
defaulting them off would only manufacture illegal instructions.

The last three rows are the RVA23 extensions that are defined to do
nothing observable. Several of them were already retiring correctly
before they had names here, because they are encoded inside another
instruction's space in a way that discards the result — `pause` is a
`fence`, the `ntl.*` hints are `c.add` into `x0`, and `prefetch.*` are
`ori` into `x0`. Claiming them properly still matters: it lets `-march`
gate them and the dashboard name them, and `Zimop`/`Zcmop` carry one real
rule — they must write **zero** to `rd`, which is the guarantee that
makes those reserved encodings safe for a future extension to claim.
`Zicbom`'s cache-management ops are satisfied trivially by a machine with
no cache; what is *not* modelled is the `menvcfg`/`senvcfg` permission
layer that lets M-mode make them trap in S/U mode.

`Zicboz` is different from its Zicbom siblings — `cbo.zero` is a real
store, and the address is aligned *down* to the 64-byte block, so
`cbo.zero (base+8)` still zeroes from `base`.

`Zawrs` retires immediately. The spec explicitly permits that, and the
software contract is built around it: the surrounding loop always re-checks
its condition, because a `wrs` may return for any reason or none. On a
single-hart interpreter it is also the only implementation that terminates,
since no other hart exists to break the reservation.

The counters are one number. `mtime` advances once per retired instruction
(that is what makes this machine's timer deterministic), so `cycle`, `time`
and `instret` all read the same counter — an interpreter that retires one
instruction per step has `cycle == instret` by construction. `hpmcounter3`
through `hpmcounter31` read as zero, which the spec permits and which is
the honest answer for a machine that counts no events.

Zfa is mostly not new arithmetic — it is arithmetic that differs from an
existing instruction only in a corner, which is what makes it worth testing
carefully. `fminm`/`fmaxm` return a canonical NaN when *either* operand is
NaN, where `FMIN`/`FMAX` return the other operand; `fleq`/`fltq` return the
same value as `FLE`/`FLT` for every input and differ only in which NaNs
raise invalid; and `fcvtmod.w.d` wraps modulo 2³² where every other
float-to-int conversion saturates. Adding it surfaced a latent bug in the
base F and D extensions, where `FMIN`/`FMAX`/`FEQ` raised invalid for a
quiet NaN when the spec reserves that for a signalling one — a documented
simplification that was harmless for results and wrong for flags, and that
the accrued-flags-only test could not see.

RVA23 mandates only `Zfhmin` and `Zvfhmin` — half precision *arithmetic*
is an expansion option — but the full `Zfh` and `Zvfh` are implemented
here anyway, so `fadd.h` and its vector twin are real instructions rather
than something software has to widen around. MinGW has no `_Float16` on
x86, so the conversions are hand-written integer code
(`src/extensions/ext_fp16.hpp`) and the arithmetic goes through Berkeley
SoftFloat's `f16` alongside F and D. Narrowing rounds exactly once from
the source significand: going double → float → half rounds twice, and a
value that is an exact midpoint in half but not in single rounds the
wrong way.

Carrying an f16 through the vector unit needed one trick worth naming.
`ext_v_fp.cpp` computes in `double` and converts at the edges, and the
obvious conversion canonicalises every NaN — which destroys the operand's
payload and quiets a signalling NaN before anything can raise invalid on
it. So a half NaN is smuggled through the double as `0x7FF8000000000000 |
bits`, where the low sixteen bits are the original encoding and the result
is still a NaN to everything in between.

Adding them surfaced two older gaps. `exec_v_fp` returned silently for
any SEW other than 32 or 64, so every vector FP instruction at `e16` was
a no-op; and the whole widening/narrowing half of VFUNARY0
(`vfwcvt.*`/`vfncvt.*`, 14 instructions) was ignored the same way. Both
are implemented now. Separately, float-to-integer conversions never
reported inexact: `collect_fflags()` reads MXCSR, but `std::llrint` on
this toolchain goes through x87, so the flag was dropped. It is now
decided from the values — a conversion is inexact exactly when its
result converts back to something different — which holds for every
rounding mode and does not depend on the host.

`Svnapot` and `Svpbmt` add no instructions — they give meaning to page
table entry bits that were previously ignored, so the only way to exercise
them is an actual page-table walk. `Svnapot`'s N bit makes the low four
bits of the physical page number come from the *virtual* address, so one
entry covers 64KB; an implementation that ignores it still translates, it
just aliases every address in the range onto the same page. `Svpbmt`'s
memory types are genuinely unobservable here — with no caches, PMA, NC and
IO behave alike — so what makes it real is what must *fault*: the reserved
type 3, and any nonzero type while `menvcfg.PBMTE` is clear, which is how
an OS probes for the extension. Bits 60:54 of a PTE are now checked as
reserved too; without that the other two would be meaningless.

`Svinval`'s three instructions retire without effect for the same reason
`fence.i` does: `mmu_translate` walks the page table in guest memory on
every access, so a translation can never be stale and there is nothing to
invalidate.

CSR accesses are privilege-checked: writing a read-only CSR, or touching
one above the current privilege level, raises an illegal instruction, and
`cycle`/`time`/`instret` are gated in S- and U-mode by the
`mcounteren`/`scounteren` chain. CSR *numbers* this machine gives no
meaning to are still readable and writable — OpenSBI detects hart features
by reading a spread of CSRs to see which trap, so making unknown ones
illegal is a much larger change than making privilege boundaries real.

Every extension is a runtime toggle, not a compile-time one — pass
`-march=rv64imafdc_zicsr_zifencei` (the default), add a `v` for vector
support, or trim it down to something like `rv32ima` if you want to see it
fail in more interesting ways. `fence.i` is implemented as a genuine no-op
rather than being unsupported: there's no instruction cache here to
invalidate, since every fetch reads straight out of live guest memory, so
the correct emulation of "make sure instruction fetches see recent stores"
is to just not need to do anything.

## Correctness

"It draws pixels that look like Doom" is not a correctness bar, so this
is measured against the architecture instead of against itself.

**Sail is the golden reference.** The [Sail RISC-V model](https://github.com/riscv/sail-riscv)
is generated from the same source the architecture is defined in, rather
than being an independent reimplementation, and it is the only model
riscv-arch-test ships an RVA23S64 configuration for -- there is no spike
RVA23S64 config at all. spike is still run behind `--ref spike`, because an
independent implementation disagreeing is a signal even when it turns out
to be the one that is wrong, but it does not decide anything.

### Live results

| suite | what it is | result |
| --- | --- | --- |
| riscv-arch-test RVA23S64 | the certification suite, 663 tests, signature-diffed against Sail | **663 / 663** |
| differential | 19 hand-written suites, 639 cases, diffed against Sail | **19 / 19** |
| riscv-vector-tests | 3042 generated V tests at VLEN=128, signature-diffed against Sail | **3042 / 3042** |
| riscv-tests | the Berkeley suite, 377 applicable of 667 | **377 / 377** |
| damo-rv-priv-ats | hypervisor, the only H coverage that exists anywhere -- 43 groups | **43 / 43 groups** |
| Linux | OpenSBI + 6.12 + busybox, ext4 root over virtio-blk | boots to an interactive shell, in a 1024x768 framebuffer console |
| Ubuntu 24.04 | 104 packages, configured by DoomV running Ubuntu's own `dpkg`, systemd as PID 1 | boots to `doomv login:` |
| DOOM | bare-metal, no OS | plays |

`riscv-vector-tests` is the suite that covers the vector ISA at the width
this machine implements, including the sub-profiles RVA23 leaves optional:
Zvfh, bf16, Zvbb/Zvbc, and the whole Zvk crypto family. arch-test does not
test any of them, which is why it read 663/663 through every bug in
[Part IX](docs/BUGS.md#part-ix). `riscv-tests` is mostly redundant with
arch-test but not entirely -- it is the only suite here that exercises
M-mode's illegal-instruction path directly, and that is where the last
failure anywhere in this project turned out to be: `mstatus.TSR` was
writable and never read, so an S-mode `SRET` returned when it should have
trapped.

The Ubuntu row is the one no conformance suite can stand in for. Nothing
about it is checked against a reference model -- it either configures a
hundred packages or it does not. Stage 1 unpacks the `.debs` on the host
with `debootstrap --foreign`, which executes no riscv64 code; stage 2 is
booted with `init=/doomv-stage2` and **DoomV** runs Ubuntu's own `dpkg`,
the maintainer scripts, and the perl and shell they fork. The usual way to
do that step is `qemu-user-static` and binfmt, which is deliberately not
what happens here: an emulator that boots Linux should be able to run the
distribution's own tooling, and if it cannot, that is a bug worth finding.
See [tools/linux/ubuntu/](tools/linux/ubuntu/README.md).

Every suite runs headless (`-ng`), which is what makes them practical to
run at all: arch-test's 663 tests take about 40 seconds and the 43-group
hypervisor suite about 10. Reproduce all of it with one command:

```sh
tools/verification/verify.sh          # build, then every suite
tools/verification/verify.sh --quick  # skip the two slow suites
tools/verification/verify.sh archtest # just one
```

First time only, to build the reference model and fetch the suites:

```sh
tools/verification/tests/archtest/setup.sh          # toolchain, Sail 0.13.1, act
tools/verification/tests/archtest/gen_reference.sh  # compile tests + Sail signatures
tools/verification/tests/suites/fetch.sh            # precompiled third-party suites
```

### What that process actually found

148 bugs, written up individually in [docs/BUGS.md](docs/BUGS.md). A few
that say something about the method:

* **Floating point had to stop using the host FPU.** Three ordinary bugs
  took the D and F families from 103 failures to 78, and then it stopped
  moving -- because RMM (round-to-nearest-ties-away) has no x86 encoding at
  all and was being approximated by round-to-nearest-even, wrong on every
  exact tie, and because host exception flags are unreliable on this
  machine. F/D now compute in Berkeley SoftFloat, which is what spike uses
  and why Sail and spike agree with each other. 103 failures to zero.
* **The hand-written suites passed the whole time.** All 19 matched Sail
  while 123 certification tests failed. They never fed a signalling NaN to
  `fcvt.d.s`, never exercised RMM, never hit a tie. Where an established
  suite covers the ground, it is better evidence than anything written to
  match one's own implementation.
* **Linux was never actually booting.** "238 lines to /bin/sh" was the
  emulator halting on an illegal instruction, which looks identical to a
  clean boot because the log simply stops. Making illegal instructions trap
  exposed it: userspace issued a `vsetivli`, the hart correctly called it
  illegal because the device tree never mentioned V, and the kernel turned
  that into SIGILL and killed init.
* **Whole features were missing and nothing noticed.** PMP did not exist.
  `mstatus.MPRV`, `mstatus.SD`, `mstatus.TVM`, `hstatus.VTSR`/`VTVM`/`VTW`
  were defined bits that nothing consulted. Physical memory attributes were
  unchecked, so a page-table walk off the end of RAM read zeros, saw an
  invalid PTE, and reported a *page* fault where the architecture requires
  an *access* fault.

The harness lives in `tools/verification/`. `tests/differential/` is the
hand-written suites, `tests/archtest/` drives riscv-arch-test, and
`tests/suites/` runs the precompiled third-party suites.

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
  uart.*                 8250-compatible serial, which is the SBI console
  virtio_blk.*           virtio-blk over MMIO: the root disk
  virtio_input.*         virtio-input over MMIO: the keyboard and the mouse
  timer.* aplic.* imsic.*  CLINT timer and the AIA interrupt controllers
  mmu.* pmp.*            Sv39/48/57 translation with a TLB, and the PMP
```

Each extension owns its own decode + execute logic in its own file under
`src/extensions/` — nothing shared gets fought over, and adding a new
extension (which happened more than once) never means touching a giant
switch statement that also handles six other things.

## Building & running

```
make
./riscv_doom.exe tools/doom/doombuild/DOOM1.WAD tools/doom/doombuild/doomv-free.elf
```

`DOOM1.WAD` (the shareware IWAD) is the only WAD checked into this repo —
it's free to redistribute. Point it at your own `DOOM.WAD`/`DOOM2.WAD` if
you own a copy, and rebuild the guest ELF from `tools/doom/doombuild/` to match.

### Command-line options

```
riscv_doom.exe <wad> <elf> [options]                      # bare-metal / test ELF
riscv_doom.exe -opensbi=<f> -kernel=<f> -dtb=<f> -initrd=<f> [options]   # Linux
```

| Option | Meaning |
| --- | --- |
| `-ng` | Headless: no SDL window, and the process exits as soon as the guest stops. Aliases: `-nogui`, `-headless`, `--headless`. |
| `-march=<isa>` | Override the enabled extension set, e.g. `rv64imafdc_zicsr_zifencei`. Without it a Linux boot gets the full RVA23S64 profile and everything else gets the `rv64imafdc_zicsr` default. |
| `-break=<hex_pc>` | Halt and dump full CPU state when the pc reaches this address. |
| `-sig=<hex_begin>:<hex_end>` | Dump this memory range to `signature.log` on halt — how the arch-test harness pulls signatures. |
| `-tohost=<hex_addr>` | Stop when the guest stores a nonzero word to this address, and write the value to `tohost.log`. This is how every bare-metal RISC-V suite reports its verdict, and it is the only stop signal for suites that export no signature symbols. |
| `-opensbi=<path>` | OpenSBI firmware ELF (`fw_jump.elf`). Any of the four Linux options selects Linux-boot mode. |
| `-kernel=<path>` | Kernel `Image`. |
| `-dtb=<path>` | Flattened device tree. |
| `-initrd=<path>` | Initramfs cpio archive. Omit it when booting from `-disk=` -- with an initramfs present the kernel runs that and never mounts the disk. |
| `-disk=<path>` | Raw disk image for the virtio-blk device. A whole-device filesystem or a partitioned image both work; the kernel finds the partition table itself. |
| `-fbdump=<path>` | Write the Linux framebuffer to this file as a binary PPM, with a non-black pixel count on stdout. Written when the run stops and periodically while it runs. |
| `-expect=<text>` | Hold the headless stdin feed until the guest's console prints this string. |
| `-input=<path>` | Replay a script of keyboard and mouse events into the guest. The only way to exercise the input devices without a window and a person -- see [Input](#input). |
| `-guidump=<path>` | Write the composed window -- dashboard included -- to this file as a binary PPM, every 60th frame. |

`-ng` is what makes the conformance suites practical. With a window open a
finished test never exits on its own and has to be killed from outside, so
every test cost its full timeout whether it passed or not; headless, the
whole 663-test arch-test run takes about 40 seconds and the 43-group
hypervisor suite about 10.

### Driving a guest from a pipe

A headless Linux boot reads host stdin into the UART, so a guest is
scriptable without a window:

```sh
printf 'mount -t proc proc /proc\necho o > /proc/sysrq-trigger\n' \
  | riscv_doom.exe -ng -expect='~ # ' \
      -opensbi=build/linux/fw_jump.elf -kernel=build/linux/Image \
      -dtb=build/linux/doomv.dtb -initrd=build/linux/initramfs.cpio
```

`-expect` is the part that is not optional. Anything typed at a guest before
its tty exists is read out of the UART by OpenSBI, handed to a console with
no line discipline yet, and dropped -- send at reset and the guest runs
`cho o > /proc/sysrq-trigger`. Waiting on a prompt the guest has actually
printed is the only correct fix; a delay is a guess that has to be
re-guessed every time the guest changes speed.

Newlines are translated to CR on the way in, because that is what pressing
return sends and `ICRNL` is what the guest's line discipline is expecting.
Feed a raw LF and the command is typed but never runs.

That example also ends the run: `sifive,test0` is in the device tree, so a
guest's poweroff reaches the emulator and the process exits 0. That matters
for anything unattended -- see
[tools/linux/ubuntu/](tools/linux/ubuntu/README.md), where the build is four
hours long and the alternative is watching it.

<a id="input"></a>
### Input

The window's keyboard and mouse reach the guest, and how depends on what is
running.

**Linux** gets two `virtio-input` devices, a keyboard and a mouse, at
`0x10100000` and `0x10101000`. They are real input devices: the keyboard
binds the VT layer's `kbd` handler, so the framebuffer console can be typed
at and logged into, and both appear as `/dev/input/event*` for anything that
wants to read them directly.

```
$ cat /proc/bus/input/devices
N: Name="DoomV Keyboard"
H: Handlers=sysrq kbd event0
B: EV=3
B: KEY=ffffffffffffffff fffffffffffffffe

N: Name="DoomV Mouse"
H: Handlers=mouse0 event1
B: EV=7
B: REL=143
```

Two devices rather than one, because the driver registers one input device
per virtio device: a single device claiming both keys and relative motion is
classified as a mouse with a hundred buttons by everything downstream.

Keys are forwarded as **scancodes**, not characters. An evdev code names a
physical key and the guest applies its own keymap, exactly as on real
hardware -- translating from keysyms instead would apply the host's layout
and then let the guest apply a second one on top. Characters are a separate
path: the serial console on `hvc0` gets them from SDL's own text input,
which resolves layout, modifiers and dead keys, plus ANSI sequences for the
arrows and navigation block and control characters for Ctrl+letter. Both
devices are fed every event, because which one a guest is listening to
depends on its `console=` setting and this side cannot know.

**DOOM** has no drivers, so it gets two MMIO registers instead:
`MMIO_MOUSE_MOVE` at `0x1000000C` (dx in bits 31-16, dy in 15-0, signed,
destructive read) and `MMIO_MOUSE_BTN` at `0x10000010` (held buttons). That
is a deliberately different shape from the key queue next door: DOOM reads
the mouse once a frame and wants "how far since I last asked", so movement
*merges* when a frame runs long instead of queueing behind it. The port
posts an `ev_mouse` from `DG_DrawFrame`, and DOOM's own `mousex`/`mousey`
path does the rest -- mouse look and menu navigation both work.

Three keys belong to the window rather than the guest:

| Key | |
| --- | --- |
| F9 | Resume from a debugger halt |
| Ctrl+Alt+G | Grab the mouse (hide the pointer, deliver deltas with no window edge to hit) |
| Ctrl+Alt+F | Give a Linux framebuffer the whole window instead of the dashboard's display box |

Ctrl+Alt rather than more function keys because a bare function key is not
free -- DOOM binds all twelve, so F10 and F11 would have cost it "quit game"
and the gamma control.

`-input=<path>` replays a script of events so none of this needs a person:

```
key 30 1        # a key down, evdev code for Linux, doomkeys.h code for DOOM
key 30 0
type echo hello # as press/release pairs, US layout
rel 40 -20      # relative mouse movement
btn left 1
wheel 1
sleep 500       # host milliseconds, not guest
```

### The display

There are two framebuffers, and which one the window shows depends on how
the machine was started.

DOOM writes its native 320x200 through `MMIO_FB`, and the window scales it
up inside a dashboard that shows registers, CSRs and a trace log alongside.

A Linux guest gets a 1024x768 linear aperture at `0x50000000`, declared to
the kernel as a `simple-framebuffer` node in the device tree. The kernel's
`simplefb` driver binds to it and `fbcon` draws a 128x48 character console
into it, which the window shows in the same box DOOM's display uses, with
the registers, CSRs and trace log still alongside. A source of a different
shape is letterboxed rather than stretched -- the box's 1.6 aspect ratio was
picked for DOOM's 320x200 -- and scaling down uses the nearest source pixel
rather than blending, because blending neighbours is exactly what destroys
the one-pixel stems in 8x16 console text.

1024x768 into an 840x525 box is still 0.68x, which is legible but not
comfortable, so **Ctrl+Alt+F** hands the framebuffer the whole window for
when reading the console is the job rather than watching the machine.

The aperture sits deliberately *outside* the device tree's memory node,
which is what stops Linux allocating over it without needing a
reserved-memory entry -- the same arrangement a carved-out framebuffer has
on real hardware. `simple-framebuffer` has no mode-setting protocol at all:
the driver takes width, height, stride and format from the device tree and
trusts them, so `tools/linux/dts/doomv.dts` and `Memory::LFB_*` have to
agree and nothing checks that they do.

The bootargs carry `console=tty0 console=hvc0`, in that order. Both consoles
are registered, so the kernel log reaches the framebuffer *and* the SBI
serial the headless harnesses read. The order decides which becomes
`/dev/console` and the last one wins, so `hvc0` last puts the shell on the
serial where a script can drive it.

Key bindings live in `controls.json` if you want to remap them. The mouse is
not in there: DOOM's own `mousebfire`/`mousebforward` defaults apply, and
mouse look works because the port posts an `ev_mouse` and DOOM already had
everything downstream of that. Press Ctrl+Alt+G to grab the pointer first --
without that it stops at the window edge, which is a short distance to turn
in.

The guest side — the actual Doom binary that runs *on* this CPU — is built
separately in `tools/doom/doombuild/`: a cross-compiled `doomgeneric` with a
small platform layer (`doomgeneric_doomv.c`, `w_file_doomv.c`, a libc
shim) that talks to DoomV's MMIO instead of a real OS.

## Scripts

The supported entry points are collected in `scripts/`:

```powershell
# Native host tools, then WSL/RISC-V tools separately.
powershell -ExecutionPolicy Bypass -File scripts/install_dependencies.ps1
powershell -ExecutionPolicy Bypass -File scripts/toolchain.ps1

# Build DoomV, Linux, or both.
python scripts/build.py doom
python scripts/build.py linux
python scripts/build.py all

# Boot either guest; --smoke proves Linux reaches BusyBox userspace.
python scripts/boot.py doom
python scripts/boot.py linux --smoke

# All suites by default, or selected suites by name.
python scripts/verify.py
python scripts/verify.py differential archtest
```

Install Ubuntu first when needed with `wsl --install -d Ubuntu`;
`toolchain.ps1` intentionally does not install or modify WSL itself. Full
options and dependency separation are in [`scripts/README.md`](scripts/README.md).

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
