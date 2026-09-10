# Root filesystem (BusyBox initramfs)

`src/` is a git submodule of [mirror/busybox](https://github.com/mirror/busybox),
pinned to tag `1_36_1`.

`initramfs.cpio` in this directory (gitignored build output, same as
the other `tools/*` build artifacts) is an uncompressed cpio archive of
a statically-linked BusyBox, loaded by DoomV as a separate blob
alongside the kernel `Image` and the DTB (see `src/doom_system.cpp`'s
`init_linux_boot`). The kernel's `rdinit=/bin/sh` bootarg (set in
`tools/linux/dts/doomv.dts`) execs BusyBox's own shell applet directly as
PID 1 -- no init script needed.

## Built via WSL, same toolchain as the kernel

Native MSYS2 can't build this either (BusyBox links against a real
libc via a `riscv64-linux-gnu-*` toolchain, same requirement as the
kernel -- see `tools/linux/linux/README.md`), so it reuses that already-
installed WSL2/Ubuntu environment rather than adding a new one:

```
wsl -d Ubuntu -- bash -c "mkdir -p /root/build && \
    rsync -a $DOOMV/tools/linux/rootfs/src/ /root/build/busybox/"

wsl -d Ubuntu -- bash -c "cd /root/build/busybox && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig"

# Static linking -- the initramfs has no libc of its own for a dynamic
# binary to find at runtime.
wsl -d Ubuntu -- bash -c "cd /root/build/busybox && \
    sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config"

# CONFIG_TC (the `tc` traffic-control applet) fails to build against
# this WSL image's newer kernel headers -- `struct tc_cbq_wrropt` and
# friends have changed shape since BusyBox 1.36.1 was written against
# them. Not needed for an interactive shell; disabled rather than
# patched, same call as OpenSBI/spike's own version-mismatch fixes
# elsewhere in this project. `oldconfig < /dev/null` re-resolves
# dependent options non-interactively (EOF on stdin means "keep
# default/existing"), same trick used for `make defconfig` itself.
wsl -d Ubuntu -- bash -c "cd /root/build/busybox && \
    sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- oldconfig < /dev/null"

wsl -d Ubuntu -- bash -c "cd /root/build/busybox && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j\$(nproc)"

# Installs the standard BusyBox applet symlinks (bin/sh, bin/ls, ...)
# into _install/ -- this staging directory *is* the initramfs's root.
wsl -d Ubuntu -- bash -c "cd /root/build/busybox && \
    make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- install"

# `cpio` isn't in this WSL image by default.
wsl -d Ubuntu -- bash -c "apt-get install -y cpio"

wsl -d Ubuntu -- bash -c "cd /root/build/busybox/_install && \
    find . | cpio -o -H newc > /root/build/initramfs.cpio"

wsl -d Ubuntu -- bash -c "cp /root/build/initramfs.cpio \
    $DOOMV/tools/linux/rootfs/"
```

No `/dev` entries are created in the archive -- the kernel's
`CONFIG_DEVTMPFS_MOUNT=y` (set in `arch/riscv/configs/defconfig`,
confirmed by reading it) auto-populates `/dev` before `rdinit` runs,
and the kernel opens the console and dup2()s it onto init's stdin/
stdout/stderr regardless of whether a `/dev/console` node exists in
the filesystem.

## The DTB's initrd addresses must match this file's actual size

`tools/linux/dts/doomv.dts`'s `/chosen` node hardcodes
`linux,initrd-start`/`linux,initrd-end` as absolute physical
addresses (`RAM_BASE + 0x2300000` through that plus this file's exact
byte length -- see `src/doom_system.cpp`'s `init_linux_boot` for where
`0x2300000` comes from). If this file is rebuilt and its size changes,
`linux,initrd-end` must be recomputed and the DTS recompiled
(`dtc -I dts -O dtb -o doomv.dtb doomv.dts`) before booting -- there's
no way to compute it from inside the DTB, so it isn't automatic.

## A partitioned disk image, for testing the shape a distribution has

`mkdisk.sh` builds a GPT-partitioned ext4 image whose root filesystem the
kernel mounts as `/dev/vda1`, rather than a bare filesystem written to the
whole device. Run it inside WSL as root -- it needs loop devices, `sgdisk`
and `mkfs.ext4`, none of which exist on the Windows side:

```
wsl -d Ubuntu -u root -- bash /mnt/z/Code/Dev/DoomV/tools/linux/rootfs/mkdisk.sh
```

The point is not that a partition table is interesting in itself. It is
that every image you can download has one, so a `-disk=` path tested only
against a bare filesystem leaves the partition-scanning path unexercised.
It does work: virtio-blk reports the right capacity, the kernel finds the
primary GPT, enumerates `vda1`, and mounts it --

```
virtio_blk virtio0: [vda] 393216 512-byte logical blocks (201 MB/192 MiB)
 vda: vda1
EXT4-fs (vda1): mounted filesystem ... r/w with ordered data mode.
VFS: Mounted root (ext4 filesystem) on device 254:1.
```

-- and the script exists so that stays true. The DTB needs
`root=/dev/vda1` rather than the `rdinit=/bin/sh` that `doomv.dts` ships;
the script prints the two commands for that when it finishes.

Boot it with **no** `-initrd`. With an initramfs present the kernel runs
that instead and never touches the block device, which looks like success
and proves nothing.

One arithmetic trap, since it cost a boot: size the filesystem to the
*partition*, not to the file. A filesystem five 4K blocks longer than its
partition mounts fine on the host, which is reading the file, and fails in
the guest with `bad geometry: block count 48896 exceeds size of device
(48891 blocks)`. `mkdisk.sh` reads the extent back out of the partition
table instead of computing it.
