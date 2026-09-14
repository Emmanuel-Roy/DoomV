# DoomV

A RISC-V CPU, built from scratch in C++, that started out booting bare-metal
DOOM and now boots OpenSBI, Linux, and Ubuntu 24.04 with a desktop.

No existing core as a reference implementation — just an instruction
decoder, a register file, a memory bus, and the RVA23S64 profile (RV64GCV
plus the extensions below, and H), with the handful of devices a
distribution needs on top: AIA interrupt controllers, a framebuffer, virtio
disks, a virtio keyboard and mouse, and a 9P folder shared with Windows.

<img width="1920" height="1080" alt="DOOM E1M1 running on DoomV, with the register file and trace log beside it" src="docs/images/doomv-hero.png" />

## Why

My Gameboy emulator was done, I'd taken a chunk of computer architecture
coursework by that point, and I had a month free before starting a co-op.
I wanted a project that would actually make me use that coursework instead
of just having sat through it, and "build a CPU and see if it can run Doom"
seemed like the right amount of stupid.

The original goal was small on purpose: get bare-metal DOOM booting, and if
it works, stop — nothing else needs to be implemented to call it done. That
happened a while ago. Everything past that point (F/D, C, V, formal
verification against a reference simulator, Linux, Ubuntu) is stuff I kept going on because
it was fun, not because the project needed it.

## What's actually implemented

With no `-march`, a bare-metal guest such as DOOM gets `RV64GC` plus `Zicsr`
and `Zifencei` — i.e. `RV64IMAFDC_Zicsr_Zifencei` — along with the
extensions below marked on by default. `V` (vector) and `H` stay opt-in
there, since Doom needs neither. A Linux boot with no `-march` gets the full
RVA23S64 profile instead, `V` included, because that is what the device
tree tells the kernel the hart has.

| Extension | Status |
|---|---|
| I (base integer) | ✅ |
| M (mul/div) | ✅ |
| A (atomics) | ✅ |
| C (compressed) | ✅ |
| F / D (single/double float) | ✅ |
| Zicsr | ✅ |
| Zifencei | ✅ (no-op — see below) |
| V (vector) | ✅ (on for Linux boots, opt-in otherwise) |
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
| Svinval (fine-grained TLB invalidation) | ✅ (flushes the whole TLB — see below) |
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

`Svinval`'s `sinval.vma` flushes the TLB, and `sfence.w.inval` and
`sfence.inval.ir` retire without effect. The TLB (`src/mmu.hpp`) is small,
direct-mapped and only ever flushed wholesale — by `sfence.vma`,
`sinval.vma`, and writes to the CSRs a translation depends on — so a
finer-grained invalidation has nothing finer to aim at, and bracketing it
has nothing to batch.

CSR accesses are privilege-checked: writing a read-only CSR, or touching
one above the current privilege level, raises an illegal instruction, and
`cycle`/`time`/`instret` are gated in S- and U-mode by the
`mcounteren`/`scounteren` chain. The CSRs of extensions this hart does not
have -- Smrnmi's, Sdtrig's triggers, the debug-mode registers -- trap, as they
do in Sail. Other CSR *numbers* this machine gives no meaning to are still
readable and writable: OpenSBI detects hart features by reading a spread of
CSRs to see which trap, so making every unknown one illegal is a much larger
change than making privilege boundaries real.

Every extension is a runtime toggle, not a compile-time one — pass
`-march=rv64imafdc_zicsr_zifencei` (the default), add a `v` for vector
support, or trim it down to something like `rv32ima` if you want to see it
fail in more interesting ways. `-march` sets what the hart supports; `misa`
is writable within that, as in Sail, so a guest can clear a letter to turn
the extension off and set it again to turn it back on. `fence.i` is implemented as a genuine no-op
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
| Linux | OpenSBI + 6.12 + busybox, ext4 root over virtio-blk | boots to an interactive shell, in a 1168x1056 framebuffer console |
| Ubuntu 24.04 | 104 packages, configured by DoomV running Ubuntu's own `dpkg`, systemd as PID 1; 270 more for the desktops | boots, logs in at the framebuffer console, and starts Openbox, XFCE or bare X |
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

It also runs a desktop, installed the same way -- by DoomV running `apt`:

| Openbox | XFCE | bare X |
|---|---|---|
| ![Openbox with an xterm, Ubuntu 24.04 on DoomV](docs/images/ubuntu-openbox.png) | ![The XFCE desktop, Ubuntu 24.04 on DoomV](docs/images/ubuntu-xfce.png) | ![Two xterms on bare X, Ubuntu 24.04 on DoomV](docs/images/ubuntu-x.png) |

<sub>Captured from DoomV's 1168x1056 Linux framebuffer with `-fbdump`.</sub>

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
  memory.*              guest RAM, WAD/ELF loader, MMIO bus (framebuffers, input, virtio devices)
  registers.*           x0-31, f0-31, v0-31, PC, CSRs, and the trace history ring
  extensions.*           the enabled-extension table + -march= parser
  riscv_decoder.*        instruction word -> decoded struct, extension gate, dispatch
  riscv_core.hpp          shared declarations for each extension's execute function
  extensions/            one file per extension (ext_i.cpp, ext_m.cpp, ext_v_*.cpp, ...)
  debugger.*             breakpoints, halt conditions, crash/signature dumps
  gui.*                  the SDL window, framebuffer scaling, and the debug dashboard
  uart.*                 8250-compatible serial, which is the SBI console
  virtio_blk.*           virtio-blk over MMIO: the root disk and the storage drives
  virtio_input.*         virtio-input over MMIO: the keyboard and the mouse
  virtio_9p.*            virtio-9p over MMIO: a 9P2000.L file server for the shared folder
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
| `-drives=<dir>` | Folder of `*.img` storage drives to attach after the root disk on a Linux boot, in name order. Defaults to `drives`; `-drives=` turns it off. See [Storage drives](#drives). |
| `-shared=<dir>` | Host folder served live to a Linux guest over virtio-9p, mount tag `shared`. Defaults to `shared`; `-shared=` turns it off. See [Shared folder](#shared). |
| `-fbdump=<path>` | Write the Linux framebuffer to this file as a binary PPM, with a non-black pixel count on stdout. Written when the run stops and periodically while it runs. |
| `-expect=<text>` | Hold the headless stdin feed until the guest's console prints this string. |
| `-input=<path>` | Replay a script of keyboard and mouse events into the guest. The only way to exercise the input devices without a window and a person -- see [Input](#input). |
| `-guidump=<path>` | Write the composed window -- dashboard included -- to this file as a binary PPM, every 60th frame. |
| `-stopat=<n>` | Stop after exactly `n` instructions and write the machine state to `crash.log`. Two runs of the same guest with the same inputs leave identical files -- see [Determinism](#determinism). |
| `-record=<path>` | Log every input the guest receives -- keys, pointer, serial bytes -- with the instruction it arrived at. |
| `-replay=<path>` | Deliver a `-record` log's input at exactly those instructions, and ignore the window, stdin and `-input`. Reproduces a recorded run instruction for instruction. |
| `-trace=<path>` | Write a trace of every instruction, register and CSR write, store and trap, in Sail's trace format. |
| `-lockstep=<path>` | Run against a reference trace -- Sail's, or an RTL simulation's -- and halt at the first record that does not match. See [Lock-stepping](#lockstep). |
| `-lockstep-strict` | With `-lockstep`, compare everything, counters, time and interrupt timing included, and take nothing from the reference. How DoomV is held to Sail. |

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

Bytes enter the UART on the CPU thread, when its 16-byte ring has room and
at an instruction count, never when the host happens to deliver them. So
stdin **redirected from a file** (`riscv_doom.exe ... < commands.txt`) is
read in full before the guest starts and produces the same run every time.
A pipe or a terminal is live -- what arrives, and when, is up to whatever is
on the other end -- so add `-record` to be able to reproduce it with
`-replay`.

Pick the needle carefully: it is matched against the raw byte stream, and a
guest's output is not plain text. systemd colourises unit names, so its
`Started getty@tty1.service` is really `Started \e[0;1;39mgetty@tty1.service`
and a needle spanning the space never matches -- the gate then waits
forever and the feed sends nothing, which looks exactly like input not
working. Something contiguous and unstyled, like a shell prompt or
`login:`, is the safe choice.

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
B: EV=f
B: KEY=70000 0 0 0 0
B: REL=140
B: ABS=3
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

The mouse is an **absolute** pointer, the way QEMU's tablet is: `ABS_X` and
`ABS_Y` in framebuffer pixels, 0-1167 by 0-1055, with only the wheels left
relative. So the guest's cursor sits exactly under the host pointer, and X
maps a coordinate to a pixel with no acceleration or scaling in between. A
relative mouse cannot promise that -- the guest integrates deltas under its
own acceleration, and the two cursors drift apart as soon as the host pointer
leaves the display.

The mouse belongs to the guest **only over its display**. Everywhere else in
the window is the dashboard, so motion there is not forwarded and neither is
a click; a button pressed over the display and released outside it still
sends its release, so the guest is never left holding it.

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
| Ctrl+Alt+G | Capture the mouse: fence the pointer inside the display area and hide it. DOOM also switches to deltas, so the view can keep turning. |
| Ctrl+Alt+F | Give a Linux framebuffer the whole window instead of the dashboard's display box |

Ctrl+Alt rather than more function keys because a bare function key is not
free -- DOOM binds all twelve, so F10 and F11 would have cost it "quit game"
and the gamma control.

`-input=<path>` replays a script of events so none of this needs a person:

```
key 30 1        # a key down, evdev code for Linux, doomkeys.h code for DOOM
key 30 0
type echo hello # as press/release pairs, US layout
rel 40 -20      # relative movement (Linux: moves the absolute pointer by that much)
abs 600 500     # Linux: put the pointer on a framebuffer pixel
btn left 1
wheel 1
sleep 500       # 500 x 10,000 instructions: about half a host second
wait 2M         # two million instructions (k, M, G)
```

<a id="drives"></a>
### Storage drives

Every `*.img` in `drives/` is attached to a Linux guest as another virtio
disk at boot -- `/dev/vdb` onwards after Ubuntu's root disk, `/dev/vda`
onwards in the BusyBox boot, in the order the names sort.

```sh
python scripts/mkdrive.py data 1G    # drives/data.img, formatted ext4, labelled "data"
python scripts/boot.py ubuntu
# in the guest:  mkdir -p /mnt/data && mount LABEL=data /mnt/data
```

The device tree always declares eight drive slots, after the input devices.
An empty slot reports virtio device ID 0, which Linux skips without a word,
so adding a drive is dropping a file in the folder -- no device tree change.
A read-only image file is attached read-only and the guest is told, so a
mount comes up read-only rather than failing on its first write. Details and
limits in [drives/README.md](drives/README.md).

<a id="shared"></a>
### Shared folder

`shared/` is shared live with a Linux guest: a file saved there from Windows
is visible in the guest immediately, and the other way round, with both
running. Inside the guest:

```sh
mkdir -p /mnt/shared
mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared
```

It is a virtio-9p device -- the mechanism QEMU's shared folders use -- with a
9P2000.L file server in the emulator answering the guest's v9fs requests
against the real directory. The kernel already has the client built in, so
nothing needed rebuilding. Windows has no owners, modes, symlinks or
case-sensitive names, so the guest sees an approximation: everything is
root's and writable, the read-only attribute clears the write bits, names
Windows cannot hold are refused rather than altered, and symlinks fail with
"Operation not supported". Links inside the folder that point outside it are
refused too. For a real Linux filesystem, use a drive instead. Details in
[shared/README.md](shared/README.md).

What the guest learns about the folder does not come from the host's clock
or disk either, so a run that uses it is as reproducible as one that does
not. A file the guest changes is timestamped with the guest's own time -- the
instruction count as nanoseconds from 2024-01-01 -- and that time is written
to the Windows file too. Inode numbers count up in the order the guest first
sees each file, listings are sorted by name, and `df` reports a fixed 1 TiB.

### The display

There are two framebuffers, and which one the window shows depends on how
the machine was started.

Both are shown the same way. The guest's display sits on the left, and one
column beside it holds the CSRs, the register file, the trace log and the
paused banner, with the same margin all the way round the window. The CSR
panel lists the ten CSRs the guest has used most across its last 1024 CSR
instructions, so it shows what the machine is doing now rather than
everything it ever touched.

<p align="center">
  <img src="docs/images/doom-gui.png" width="49%" alt="DOOM running on DoomV, with the register file and trace log beside it">
  <img src="docs/images/linux-gui.png" width="49%" alt="Linux booting on DoomV's framebuffer console, stopped at a breakpoint">
</p>
<p align="center"><sub>Left: DOOM. Right: Linux 6.12's boot log on the framebuffer console, stopped at a breakpoint, which is what the banner under the trace log is for.</sub></p>

Three threads besides the CPU's draw the window, and none of them waits for
another:

* **The display thread** takes whole frames out of the guest's framebuffer.
  Neither framebuffer can say "this frame is done" -- `simple-framebuffer`
  has no page flip and DOOM's is written in place -- but a frame is drawn in
  one burst of writes and then the guest goes and does something else. So
  the thread waits until the writes have stopped for 12 ms, copies, and
  throws the copy away if a write landed while it was copying. The window
  shows finished frames, not the next one being drawn a band at a time. A
  guest that draws for half a second without pausing gets that frame shown
  as it stands, so the picture never freezes.
* **The dashboard thread** draws the CSRs, register file and trace log into
  a layer of their own, from a small register snapshot the CPU thread
  publishes after every burst. The snapshot no longer carries any pixels.
* **The window thread** owns SDL. It polls input and composites the two
  layers when one of them has changed, paced by VSYNC.

DOOM writes its native 320x200 through `MMIO_FB`, and the window scales it
up to fill the display area while keeping its shape.

A Linux guest gets a 1168x1056 linear aperture at `0x50000000`, declared to
the kernel as a `simple-framebuffer` node in the device tree. The kernel's
`simplefb` driver binds to it and `fbcon` draws a 146x66 character console
into it, which the window shows at exactly 1:1 -- unscaled, so every glyph
on it is exactly the kernel's. The display area is that size for every
guest, which is why DOOM's is too.

That layout is drawn in real pixels and needs a 1920x1080 window. Below
that the dashboard falls back to an older, scaled layout, with the display
letterboxed into a smaller box. **Ctrl+Alt+F** hands a Linux framebuffer the
whole window in either case.

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
without that it stops at the edge of the display, which is a short distance
to turn in.

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

# Boot a guest. --smoke proves Linux reaches BusyBox userspace; --login
# proves Ubuntu logs in through the emulated keyboard.
python scripts/boot.py doom
python scripts/boot.py linux
python scripts/boot.py linux --smoke
python scripts/boot.py ubuntu
python scripts/boot.py ubuntu --login

# Ubuntu desktops: install all three once (hours), then pick one per boot.
python scripts/boot.py ubuntu --install-desktops
python scripts/boot.py ubuntu --desktop openbox    # or xfce, or x

# A storage drive for Linux. shared/ needs no setup.
python scripts/mkdrive.py data 1G

# All suites by default, or selected suites by name.
python scripts/verify.py
python scripts/verify.py differential archtest
```

Install Ubuntu first when needed with `wsl --install -d Ubuntu`;
`toolchain.ps1` intentionally does not install or modify WSL itself. Full
options and dependency separation are in [`scripts/README.md`](scripts/README.md).

## Design notes

<a id="determinism"></a>
**Determinism.** The same guest with the same inputs executes the same
instructions in the same order on every run, whatever the host is doing.
Guest time is the instruction count, not the wall clock, and every device
request -- a disk read, a 9P call, delivering a key -- completes, writes
guest memory and raises its interrupt on the CPU thread at the instruction
that asked for it. Nothing the guest can observe is left to host timing.

Everything the guest cannot observe runs elsewhere: the display, the
dashboard, console output (queued and written by a thread of its own), and
the periodic `-fbdump` refresh. `-stopat=<n>` is how that is checked: run the
same boot twice, or with two builds, and the `crash.log` files -- registers,
CSRs, the instruction count and the last 4096 instructions -- must be
identical byte for byte.

Input follows the same rule. Every key, pointer event and serial byte is
committed on the CPU thread at an instruction count that is a multiple of
4096, however it was produced. An `-input` script runs on that clock --
`sleep` is a fixed number of instructions, not host time -- and stdin from a
file is fed as the guest makes room for it, so both give the same run every
time. A person at the window, or a live pipe, decides what arrives outside
the machine; `-record` logs exactly what was committed and at which
instruction, and `-replay` commits it again at the same instructions, which
reproduces that session.

The shared folder is the last place the host could leak in, and it is
closed the same way: timestamps for anything the guest changes come from the
instruction count, inode numbers from the order the guest sees files, and
free space is a fixed figure. The folder's starting contents are an input,
like a disk image; given the same contents, the guest sees the same folder.

<a id="lockstep"></a>
**Lock-stepping against Sail.** That determinism is what makes DoomV
checkable one instruction at a time, and the reference it is held to is
**Sail**, the RISC-V model generated from the same source the architecture
is specified in. `-trace=<path>` writes DoomV's run in the trace format
`sail_riscv_sim` prints with `--trace-instr --trace-gpr --trace-fpr
--trace-vreg --trace-csr --trace-mem --trace-exception --trace-interrupt`:
a line per instruction, then a line per effect.

```
[36] [M]: 0x00000000800000E0 (0x30529073)
CSR mtvec (0x305) <- 0x00000000800000E8
[83] [U]: 0x00000000800001BC (0x00113023)
mem[W,0x0000000080002000] <- 0x00AA00AA00AA00AA
[37] [M]: 0x00000000800000E4 (0x74445073)
trapping from M to M to handle illegal-instruction
handling exc#illegal-instruction at priv M | tval=0x0000000074445073 | tval2=0x0000000000000000 | tinst=0x0000000000000000
CSR mcause (0x342) <- 0x0000000000000002
```

`-lockstep=<path>` runs DoomV against a reference trace in that format --
Sail's, or an RTL testbench's written the same way -- one record at a time.
It stops at the first record that differs, prints both records and the field,
writes `crash.log`, and exits 1 when headless:

```
lockstep: MISMATCH at reference line 177, after 56 matching records (instruction 57)
  CSR mideleg (0x303): reference 0x0000000000001444, DoomV 0x0000000000000444
  reference:
    [56] [M]: 0x000000008000012C (0x30305073) csrrwi x0, mideleg, 0x0     reset_vector+220
    CSR mideleg (0x303) <- 0x0000000000001444
  DoomV:
    [56] [M]: 0x000000008000012C (0x30305073)
    CSR mideleg (0x303) <- 0x0000000000000444
```

Compared, for an instruction that completes: privilege (with V), pc and
instruction bits; every register and CSR write, against DoomV's value after
it; every store, by physical address and value; and that DoomV wrote nothing
the reference did not. For a trap or an interrupt: the same cause, epc and
tval, and the same values in every CSR trap entry writes.

With `-lockstep-strict` that is everything: reads of the counters and the
time, pending-interrupt state, and interrupts, which DoomV has to take by
itself at the instruction the reference took them. Without it, lock-step is
lenient about what an implementation's own clock and devices decide, so an
RTL design with a different timer can still be stepped: those reads (`mip`,
`sip`, the `topi`/`topei` registers, the counters and the time), and loads
from anything that is not RAM, are taken from the reference, and interrupts
are taken exactly where the reference took them, never on DoomV's own. A log
in Spike's `--log-commits` format is also read, for a reference that only
produces that.

`tools/verification/lockstep_sail.py` does this for the riscv-tests: Sail
traces each test, and DoomV lock-steps against the trace, strictly. Sail runs
with the suites' `rva23s64.json` exactly as it is: the goal is to match Sail
deterministically, so DoomV is the one that has to be that hart -- 63 guest
external interrupt lines, vectored trap vectors, a writable `misa` -- and the
reference is never adjusted to fit it.

```sh
python tools/verification/lockstep_sail.py                  # every rv64 test
python tools/verification/lockstep_sail.py rv64mi-p-csr     # one
```

It found six differences on its first run that every conformance suite had
passed over -- see [Part XIII of the bug history](docs/BUGS.md#part-xiii).

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
