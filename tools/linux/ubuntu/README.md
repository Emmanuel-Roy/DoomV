# Ubuntu root filesystem

`src/` is a git submodule of
[Ubuntu's debootstrap source](https://git.launchpad.net/ubuntu/+source/debootstrap),
pinned to the `applied/ubuntu/noble` branch — Ubuntu's packaging rather than
Debian's, because the suite scripts differ and `scripts/noble` only exists in
Ubuntu's tree.

`mkrootfs.sh` builds `ubuntu.img`, a GPT-partitioned ext4 disk image holding
an Ubuntu 24.04 riscv64 root filesystem with systemd as PID 1. Both are
gitignored build output, same as every other `tools/*` artifact.

## Why this is separate from `../rootfs/`

`../rootfs/` builds a BusyBox initramfs: one static binary, exec'd directly
as PID 1 by `rdinit=/bin/sh`. It proves the kernel boots. It proves almost
nothing about the machine underneath, because BusyBox's shell asks the kernel
for very little.

Ubuntu asks for a great deal. systemd needs cgroups, epoll, signalfd,
timerfd, inotify, mount namespaces, a working monotonic clock and a real
`/proc` and `/sys`; the package tooling needs a filesystem it can actually
write; and all of it runs as several hundred processes rather than one. A
machine that boots BusyBox is not thereby known to boot a distribution, and
the gap between the two is where the interesting emulator bugs live.

Keeping it in its own directory with its own submodule follows the pattern
the rest of `tools/` already uses: one third-party source pinned per
component, with DoomV's own build script beside it rather than inside it.

## Building, in two stages

Stage 1 unpacks; stage 2 configures. The split is not a convenience -- it is
how cross-architecture bootstrapping works, and it is where the interesting
decision is.

```
git submodule update --init tools/linux/ubuntu/src
wsl -d Ubuntu -u root -- apt-get install -y gdisk
wsl -d Ubuntu -u root -- bash /mnt/z/Code/Dev/DoomV/tools/linux/ubuntu/mkrootfs.sh
tools/linux/ubuntu/boot-stage2.sh
```

**Stage 1** (`mkrootfs.sh`, on the host) runs `debootstrap --foreign`, which
unpacks the `.debs` and executes nothing from the target architecture. That is
precisely what makes it possible on an x86 machine. It downloads from
`ports.ubuntu.com` -- riscv64 is a *ports* architecture and is not on
`archive.ubuntu.com`.

**Stage 2** (`boot-stage2.sh`) configures those packages, which means running
riscv64 `dpkg`, its maintainer scripts, and the perl and shell those fork.
**DoomV runs it.** The image is booted with `init=/doomv-stage2` and the
emulator does the work.

The ordinary way to do this is `qemu-user-static` plus binfmt, letting the
host execute the target's binaries. That is deliberately not what happens
here, and not because qemu is unavailable: the point of an emulator that
boots Linux is that it can run the distribution's own tooling. If DoomV can
configure a hundred Ubuntu packages then it is running real riscv64 userspace
under real load, which is a far stronger statement than any conformance suite
makes -- and if it cannot, that is a bug worth finding. Expect it to take a
long while -- a Linux boot measures about 6.6 MIPS
(`tools/verification/bench_boot.sh`), and this is a great deal of dpkg.

Stage 2 is unattended, and it took three separate fixes to make that true.
The guest powers itself off through SBI SRST and the `sifive,test0` device in
the device tree -- without that device a guest's `poweroff` returns "not
supported" and the machine sits at a dead prompt, which is why the device
exists at all. But `poweroff` in an Ubuntu rootfs is systemd's and wants to
talk to a running systemd, which there is not when the stage-2 script is PID
1, so the script goes through `/proc/sysrq-trigger` instead; that needs
`CONFIG_MAGIC_SYSRQ`, which riscv `defconfig` leaves off and
`scripts/build_linux.sh` now enables; and the emulator had to actually *read*
the register it had been acknowledging writes to. See
[Part XI of BUGS.md](../../../docs/BUGS.md#part-xi-toc).

`boot-stage2.sh` does not depend on any of that. It runs the emulator in the
background and ends the run itself when the completion marker appears in the
log, so a four-hour build does not hinge on which kernel happens to be in
`build/linux`. The marker is printed after the guest's `sync`, so it means
"the image is on disk" rather than "the script got this far".

### Checking out a POSIX tool on Windows breaks it twice

Both of these are handled by the script, and both are worth knowing about
because they apply to any vendored shell tool, not just this one. The parent
`.gitattributes` deliberately leaves `tools/*/src` alone -- submodules keep
whatever upstream uses -- and upstream debootstrap ships no `.gitattributes`
of its own, so it arrives from a Windows clone damaged in two independent
ways.

**Line endings.** `core.autocrlf=true` gives every file CRLF, and debootstrap
is POSIX shell:

```
debootstrap: line 2: set: -: invalid option
```

**Symlinks.** Git on Windows cannot create them without developer mode, so it
writes each one out as a one-line text file containing the target's name. 47
of debootstrap's 70 suite scripts are symlinks -- `scripts/noble` is the five
bytes `gutsy` -- so sourcing one runs its target's *name* as a command:

```
scripts/noble: gutsy: not found
```

That one is genuinely nasty, because debootstrap has already redirected its
own output by the time it happens: the visible symptom is an exit status of
127 and complete silence. Finding it took running debootstrap under `sh -x`
and reading the last twenty lines of the trace.

`mkrootfs.sh` works from a copy in a temp directory with CRs stripped and the
symlinks materialised, rather than requiring every checkout to be configured
correctly.

### Host dependencies debootstrap does not check

`zstd`, in particular. Noble compresses `.deb` payloads with it, a WSL
install does not ship it, and debootstrap's failure mode is another bare exit
127 from inside the unpack loop with no indication of which tool was missing.
`mkrootfs.sh` checks `wget ar gpgv xz zstd` up front and names the package to
install.

## Booting

```
python scripts/boot.py ubuntu            # window, systemd, log in as root
python scripts/boot.py ubuntu --no-build # skip the kernel rebuild
python scripts/boot.py ubuntu --headless # no window; kernel log on stdout
python scripts/boot.py ubuntu --login    # headless self-check, see below
```

`tools/linux/ubuntu/boot.sh` does the same thing from MSYS2/Git Bash and
compiles its own device tree through WSL; the Python script uses the
`ubuntu.dtb` that `scripts/build_linux.sh` already produces, so it needs no
WSL round trip to start.

`--login` is the unattended check: it boots headless, waits for the login
prompt, and then types a username, a password and a command **through the
emulated keyboard**, so it exercises the virtio-input device and the VT
layer rather than just the boot. It leaves `build/logs/ubuntu-login.log` and
a framebuffer dump beside it.

Log in as `root` / `doomv`. That opens a window, and with `FB_SIMPLE` in the
kernel and the `framebuffer@50000000` node in the device tree the console is
a real 1024x768 framebuffer rather than a serial log -- Ubuntu being
*displayed* by the emulator, not merely logged by it. Pass `-ng` for the log
instead.

You can type at it, too: the device tree carries a `virtio-input` keyboard
and mouse, and the keyboard binds the VT layer's own handler, so `doomv
login:` on tty1 is a prompt rather than a picture of one. Ctrl+Alt+F gives
the framebuffer the whole window if the console in the dashboard's display
box is too small to read comfortably; Ctrl+Alt+G grabs the mouse. See
[Input](../../../README.md#input).

Two things not to do:

* **No `-initrd`.** With an initramfs present the kernel runs that instead
  and never mounts the disk, which looks like a successful boot of nothing.
  Both scripts strip the `linux,initrd-*` properties rather than leaving them
  pointing at a stale address, because a `/chosen` that claims an initramfs
  that was never loaded is how you get a kernel unpacking garbage.
* **No `init=` override** after stage 2. `init=/bin/sh` reaches a shell far
  faster and skips systemd entirely, which is the whole point of this image.

## Known constraints

**Memory.** DoomV currently has 1 GB (`Memory::RAM_SIZE` in
`src/memory.hpp`, mirrored by the `memory@80000000` node in
`tools/linux/dts/doomv.dts` — the two have to agree). systemd in 1 GB is
workable but tight, and `apt` is not. If the boot dies in the OOM killer,
that pair is the thing to raise, and both must be changed together.

**A guest-side timer is not a wall clock.** Guest time here is driven by
retired instructions -- `mtime` advances once per instruction and
`timebase-frequency` is 1e9 -- so one guest second is a billion instructions,
which is minutes of real time. A `sleep 60` inside the guest is a
three-and-a-half *hour* wait, which is how the first version of the stage-2
heartbeat produced no output at all. Anything scripted inside a guest that
means to wait for a wall-clock interval has to be scaled, or keyed off work
done rather than time passed.

**Speed.** A Linux boot measures about 6.6 MIPS -- run
`tools/verification/bench_boot.sh` to check it on your machine -- so a
systemd boot that takes two seconds on hardware takes minutes here. The
figure is worth measuring rather than assuming: it was 1.67 MIPS before a
TLB, a `read16` fast path and a PMP region cache, which is a 4x difference
in how long anything on this page takes. That is expected, not a fault, and
it is why `-ng` exists for the test suites — but for this image you want the
window, since the point is to log in and look around.

**No network.** There is no NIC in this machine, so `apt` cannot reach a
mirror from inside the guest; everything the image needs has to be in it
before it boots. `mkrootfs.sh`'s `--include=` list is where to add packages.
`systemd-networkd-wait-online` is disabled by the script for the same
reason — left enabled it blocks the boot for two minutes waiting for a link
that will never come up.

## Status

Built and booted. Stage 1 unpacked 104 packages on the host; DoomV ran stage 2
and `dpkg` configured all 104 of them -- `dpkg -l` on the finished image shows
104 `install ok installed` and nothing left unpacked, and `/sbin/init` is
systemd. Booting that image reaches `Welcome to Ubuntu 24.04 LTS!`, `Reached
target multi-user.target` and a `doomv login:` prompt.

The framebuffer is verified the same way, with `-fbdump=<path>` rather than a
screenshot -- a screenshot of an SDL window is not evidence, since
`CopyFromScreen` captures whatever is actually on top of that rectangle and
`PrintWindow` returns black for GPU-composited content. The dump comes from
`Memory::linux_framebuffer()` itself, so it cannot be the wrong window. On the
Ubuntu boot it holds two lines of 8x16 fbcon text at the top left:

```
Ubuntu 24.04 LTS doomv tty1

doomv login: _
```

which is getty's issue banner, rendered by the kernel into DoomV's
framebuffer. Its pixel count is small (about a thousand lit pixels of
786432) because that is all a cleared console with a login prompt on it
*is* -- worth knowing before concluding from a thumbnail that the screen is
blank.

And it is a prompt rather than a picture of one. With the `virtio-input`
keyboard in the device tree, a scripted login reaches a root shell and runs
a command, all of it through the emulated keyboard and all of it read back
out of the framebuffer:

```
tools/linux/ubuntu/boot.sh   # or, scripted and headless:
riscv_doom.exe -ng -expect='doomv login:' -input=login.script \
  -fbdump=fb.ppm -kernel=build/linux/Image -dtb=... -disk=ubuntu.img
```

```
doomv login: root
Password:
  ... the Ubuntu MOTD ...
root@doomv:~# uname -srm
Linux 6.12.0 riscv64
root@doomv:~#
```

Two things about scripting that guest. `-expect` is matched against the raw
byte stream, and systemd colourises unit names -- `Started
getty@tty1.service` is really `Started \e[0;1;39mgetty@tty1.service`, so a
needle spanning the space never fires and the script silently sends
nothing. `doomv login:` is contiguous and safe. And the waits have to be
generous: authenticating a password means `crypt()`, which at 6.6 MIPS is
not quick, and typing during it gets echoed by the tty before `login` has
finished, which looks alarming and is harmless.
