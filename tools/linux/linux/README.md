# Linux kernel

`src/` is a git submodule of [torvalds/linux](https://github.com/torvalds/linux),
pinned to tag `v6.12` (shallow -- `.gitmodules` sets `shallow = true` so
future clones/updates only fetch that one commit, not kernel history).

## Building it

```
python scripts/build.py linux
```

runs [`scripts/build_linux.sh`](../../../scripts/build_linux.sh) inside WSL.
It fetches the pinned commit onto WSL's own filesystem, builds `Image` for
`ARCH=riscv` (entry point `0xffffffff80000000`, the standard Sv39 high-half
mapping), and copies it to `build/linux/Image` beside the firmware, the
BusyBox archives and the compiled device trees.

The configuration is `defconfig` with these options enabled on top, set with
`scripts/config` and resolved with `olddefconfig`:

| Option | Why |
| --- | --- |
| `RISCV_SBI_V01` | The console. See below. |
| `NONPORTABLE`, `HVC_RISCV_SBI` | `hvc0`. See below. |
| `BLK_DEV_INITRD`, `BINFMT_SCRIPT` | The BusyBox initramfs, and a shell script as init. |
| `FB`, `FB_SIMPLE`, `FRAMEBUFFER_CONSOLE` | The graphical console. riscv `defconfig` builds the fbdev core but no framebuffer driver, so the `simple-framebuffer` node would bind to nothing and the console would be `colour dummy device 80x25`. |
| `VIRTIO_INPUT`, `INPUT_EVDEV` | The keyboard and mouse. fbcon takes its keystrokes from an input device, not the serial port, so without these the window shows a login prompt nobody can type at; X reads `/dev/input/event*`. |
| `MAGIC_SYSRQ` | `echo o > /proc/sysrq-trigger`, the only power-off available to an init that is not systemd -- Ubuntu's stage-2 build, for one. The kernel then calls SBI SRST, which reaches DoomV's `sifive,test0` device. |

virtio-blk, virtio-9p, ext4 and the AIA interrupt-controller drivers are
already in `defconfig`.

## Why `RISCV_SBI_V01`

OpenSBI hardcodes `SBI_ECALL_VERSION_MAJOR/MINOR = 1.0`
(`tools/linux/opensbi/src/include/sbi/sbi_ecall.h`): it always reports SBI
spec v1.0 through the BASE extension, regardless of what it implements. It
does implement SBI DBCN (`lib/sbi/sbi_ecall_dbcn.c`), but Linux only tries
DBCN when the *reported* spec version is at least 2.0
(`arch/riscv/kernel/sbi.c`). So against this OpenSBI that path never
activates, and both `drivers/tty/serial/earlycon-riscv-sbi.c` and
`drivers/tty/hvc/hvc_riscv_sbi.c` fall through to legacy SBI v0.1
`console_putchar`, which needs this option (off in `defconfig`). Without it
the kernel boots with a completely silent console -- not a hang, just no
output the entire time.

## Why `NONPORTABLE` and `HVC_RISCV_SBI`

The bootargs in `tools/linux/dts/doomv.dts` ask for `console=hvc0`, and hvc0
only exists if the SBI console driver is built in. It is not reachable from
`defconfig`: `drivers/tty/hvc/Kconfig` gates it on
`depends on RISCV_SBI && NONPORTABLE`, and `NONPORTABLE` is off by default,
so the symbol is never even offered and is *absent* from `.config` rather
than "is not set". `RISCV_SBI_V01` is necessary but not sufficient: it only
gets the earlycon. Without hvc0 the bootconsole is never replaced, so the
log stops the moment the real console would take over. With it, the
handover is visible:

```
printk: legacy console [hvc0] enabled
printk: legacy bootconsole [sbi0] disabled
```

`NONPORTABLE` has to be set before `HVC_RISCV_SBI` can be selected at all.

## Why `Image` and not `all`

The default `all` target also builds device trees for every vendor board the
kernel supports (SiFive, Microchip, Canaan, Allwinner, ...), several of which
fail under `-jN` with missing dt-bindings headers -- an upstream
dependency-ordering issue in parallel dtbs builds, unrelated to this
project. DoomV has its own device tree and needs none of them.

## Why WSL, not MSYS2

Configuring the kernel directly under MSYS2 fails immediately
(`fixdep: read: No error`) -- an old, well-documented incompatibility
between Kbuild's `fixdep` host tool and Cygwin/MSYS2-style POSIX emulation,
never fixed upstream. WSL2 sidesteps it, since it is a real Linux kernel
underneath.

The source also stays on WSL's own ext4 filesystem rather than the Windows
checkout. A 90k-file tree across the `/mnt/...` boundary is dramatically
slower, and on Windows the `tools/linux/linux/src` submodule may not be
checkoutable at all: the tree contains
`drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c`, its `.h`, and
`include/soc/arc/aux.h`, and `AUX` is a reserved DOS device name, so git's
`core.protectNTFS` (on by default on Windows) refuses them:

```
error: invalid path 'drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c'
```

`build_linux.sh` therefore fetches the pinned commit by SHA inside WSL and
copies only `Image` back out. `.gitmodules` marks the submodule shallow but
pins no branch, so a plain `--depth 1` clone would fetch today's master tip,
not v6.12.
