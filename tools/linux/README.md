# Linux kernel

`src/` is a git submodule of [torvalds/linux](https://github.com/torvalds/linux),
pinned to tag `v6.12` (shallow -- `.gitmodules` sets `shallow = true` so
future clones/updates only fetch that one commit, not kernel history).

`Image` and `vmlinux` in this directory (gitignored build output, same
as the other `tools/*` build artifacts) are `defconfig` builds for
`ARCH=riscv`, entry point `0xffffffff80000000` (standard Sv39 kernel
high-half mapping), with one config override on top of defconfig:
`CONFIG_RISCV_SBI_V01=y` (see below).

## Built via WSL, not natively on this machine

Configuring/building directly under MSYS2 on this Windows dev machine
fails immediately (`fixdep: read: No error`) -- a genuinely old,
well-documented incompatibility between Kbuild's `fixdep` host tool and
Cygwin/MSYS2-style POSIX emulation on native Windows, with reports
going back to at least 2005-2007 kernel mailing list threads and never
properly fixed upstream. Unlike the OpenSBI/spike build issues (a
few-years-old codebase vs. a newer GCC default, or a Windows-can't-
checkout-a-real-symlink quirk -- both genuinely fixable), this is a
structural gap in how Kbuild's host tools do low-level file I/O under
MSYS2's POSIX-on-Win32 emulation, not worth patching around locally.

Built instead inside WSL2 (Ubuntu), which sidesteps the issue entirely
since it's a real Linux kernel underneath, not emulated POSIX:

```
wsl --install -d Ubuntu --no-launch   # one-time setup
wsl -d Ubuntu -- bash -c "apt-get update && apt-get install -y \
    build-essential flex bison bc libssl-dev libelf-dev git rsync \
    gcc-riscv64-linux-gnu"

# Build on WSL's own ext4 filesystem, not the /mnt/c/... Windows mount --
# a 90k-file kernel tree checked out via git or copied via rsync across
# the 9p/drvfs boundary is dramatically slower there than natively.
wsl -d Ubuntu -- bash -c "mkdir -p /root/build && \
    rsync -a $DOOMV/tools/linux/src/ /root/build/linux/"

wsl -d Ubuntu -- bash -c "cd /root/build/linux && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig"

# CONFIG_RISCV_SBI_V01=y (Stage 4): OpenSBI hardcodes
# SBI_ECALL_VERSION_MAJOR/MINOR = 1.0 (tools/opensbi/src/include/sbi/
# sbi_ecall.h) -- it always reports SBI spec v1.0 via the BASE extension,
# regardless of what it actually implements (it does implement SBI DBCN,
# confirmed via lib/sbi/sbi_ecall_dbcn.c). Linux only tries DBCN when the
# *reported* spec version is >= 2.0 (arch/riscv/kernel/sbi.c), so against
# this OpenSBI version that path never activates -- both
# drivers/tty/serial/earlycon-riscv-sbi.c and drivers/tty/hvc/
# hvc_riscv_sbi.c fall through to legacy SBI v0.1 console_putchar
# instead, which needs this config on (off by default in defconfig).
# Without it, the kernel boots with a completely silent, unusable
# console -- not a hang, just zero output the entire time.
wsl -d Ubuntu -- bash -c "cd /root/build/linux && \
    sed -i 's/# CONFIG_RISCV_SBI_V01 is not set/CONFIG_RISCV_SBI_V01=y/' .config && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig < /dev/null"

# CONFIG_NONPORTABLE=y + CONFIG_HVC_RISCV_SBI=y: the bootargs in
# tools/dts/doomv.dts ask for `console=hvc0`, and hvc0 only exists if the
# SBI console driver is built in. It is NOT reachable from defconfig:
# drivers/tty/hvc/Kconfig gates it on `depends on RISCV_SBI && NONPORTABLE`,
# and NONPORTABLE is off by default -- so the symbol is never even offered
# and shows up *absent* from .config rather than as "is not set".
# CONFIG_RISCV_SBI_V01 above is necessary but not sufficient: it only gets
# you the earlycon (earlycon=sbi / SERIAL_EARLYCON_RISCV_SBI, already in
# defconfig). Without hvc0 the bootconsole is never replaced, so the log
# stops the moment the real console would take over and rdinit=/bin/sh has
# no console at all. With it, the handover is visible in the boot log:
#     printk: legacy console [hvc0] enabled
#     printk: legacy bootconsole [sbi0] disabled
# scripts/config is used rather than sed because NONPORTABLE has to be set
# before HVC_RISCV_SBI can be selected at all.
wsl -d Ubuntu -- bash -c "cd /root/build/linux && \
    ./scripts/config --enable NONPORTABLE --enable HVC_RISCV_SBI && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig < /dev/null"

# `Image` specifically, not the default `all` target -- `all` also
# tries to build device trees for every vendor board the kernel
# supports (SiFive, Microchip, Canaan, Allwinner, ...), several of
# which fail under -jN with missing dt-bindings headers (a real
# upstream dependency-ordering issue in parallel dtbs builds, unrelated
# to anything in this project) -- none of which DoomV needs anyway,
# since Stage 3 will author its own device tree from scratch.
wsl -d Ubuntu -- bash -c "cd /root/build/linux && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- Image -j\$(nproc)"

wsl -d Ubuntu -- bash -c "cp /root/build/linux/arch/riscv/boot/Image \
    /root/build/linux/vmlinux $DOOMV/tools/linux/"
```

Note: the rsync'd copy in `/root/build/linux` has a dangling `.git`
gitlink (it's a relative-path pointer into the *Windows-side*
superproject's `.git/modules/`, which doesn't resolve inside WSL's
independent filesystem) -- cosmetic only, doesn't affect the build,
just means `git` commands don't work in that copy.


## Where the kernel source lives

`$DOOMV` above is this repo's checkout as seen from inside WSL (e.g.
`/mnt/z/Code/Dev/DoomV`) -- set it once per shell rather than hardcoding
one machine's path.

Note that on Windows the `tools/linux/src` submodule may not be checkoutable
at all: the tree contains `drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c`,
its `.h`, and `include/soc/arc/aux.h`, and `AUX` is a reserved DOS device
name, so git's `core.protectNTFS` (on by default on Windows) refuses them:

```
error: invalid path 'drivers/gpu/drm/nouveau/nvkm/subdev/i2c/aux.c'
```

Simplest route is therefore to skip the Windows-side submodule entirely and
clone the pinned commit directly inside WSL, then copy only `Image` and
`vmlinux` back out. That also drops the rsync above -- which existed only
because the source lived on the Windows side -- and with it the slow 9p
crossing for a 90k-file tree. Note `.gitmodules` marks the submodule
shallow but pins no branch, so a plain `--depth 1` clone fetches today's
master tip, not v6.12; fetch the pinned SHA explicitly.