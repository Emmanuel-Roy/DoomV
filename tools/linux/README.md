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
    rsync -a /mnt/c/Users/royem/SWE/GitHub/DoomV/tools/linux/src/ /root/build/linux/"

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
    /root/build/linux/vmlinux /mnt/c/Users/royem/SWE/GitHub/DoomV/tools/linux/"
```

Note: the rsync'd copy in `/root/build/linux` has a dangling `.git`
gitlink (it's a relative-path pointer into the *Windows-side*
superproject's `.git/modules/`, which doesn't resolve inside WSL's
independent filesystem) -- cosmetic only, doesn't affect the build,
just means `git` commands don't work in that copy.
