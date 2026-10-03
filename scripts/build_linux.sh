#!/usr/bin/env bash
# WSL: build the pinned firmware, kernel and BusyBox; package both boot modes.
set -euo pipefail
ROOT="${1:?repository path}"
source "$(dirname "$0")/common_wsl.sh"
for tool in make riscv64-linux-gnu-gcc dtc cpio python3; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; run scripts/install_dependencies.ps1" >&2; exit 2; }
done
OUT="$ROOT/build/linux"
mkdir -p "$OUT"

echo '=== OpenSBI (pinned gitlink) ==='
SBI="$(pinned_source opensbi tools/linux/opensbi/src https://github.com/riscv-software-src/opensbi.git)"
# As many harts as the machine can have. v1.3 sizes every per-hart table by
# SBI_HARTMASK_MAX_BITS, 128, and a hart past it corrupts memory (256 harts
# faulted in imsic_map_hartid_to_data). DoomV's CLINT serves harts 0..4094,
# as Sail's does, so 4096 covers any machine -harts can make. The edit is to
# the build's copy of the pinned source, never the submodule.
sed -i 's/^#define SBI_HARTMASK_MAX_BITS\t\t[0-9]*$/#define SBI_HARTMASK_MAX_BITS\t\t4096/' \
    "$SBI/include/sbi/sbi_hartmask.h"
grep -qP 'SBI_HARTMASK_MAX_BITS		4096' "$SBI/include/sbi/sbi_hartmask.h" \
    || { echo "could not raise SBI_HARTMASK_MAX_BITS" >&2; exit 1; }
# A hart mask is then 512 bytes, and each hart's scratch area holds a TLB
# shootdown queue of 8 entries that carry one ("tlb init failed (error
# -1006)" with the default 4KB). Scratch sits at the top of the hart's stack
# area, so both grow: 8KB of scratch in 16KB, leaving the 8KB of stack.
sed -i 's/^#define SBI_SCRATCH_SIZE\t\t\t(0x[0-9a-f]*)$/#define SBI_SCRATCH_SIZE\t\t\t(0x2000)/' \
    "$SBI/include/sbi/sbi_scratch.h"
sed -i 's/^#define SBI_PLATFORM_DEFAULT_HART_STACK_SIZE\t[0-9]*$/#define SBI_PLATFORM_DEFAULT_HART_STACK_SIZE\t16384/' \
    "$SBI/include/sbi/sbi_platform.h"
grep -qP 'SBI_SCRATCH_SIZE\t\t\t\(0x2000\)' "$SBI/include/sbi/sbi_scratch.h" \
    && grep -qP 'HART_STACK_SIZE\t16384' "$SBI/include/sbi/sbi_platform.h" \
    || { echo "could not resize OpenSBI's per-hart areas" >&2; exit 1; }
# v1.3 implements the SBI debug console (DBCN) but reports spec 1.0, and
# Linux only looks for DBCN under 2.0 or later, so the console used to need
# the legacy SBI v0.1 calls -- which cap Linux at 64 CPUs. It reports 2.0
# instead. Linux probes every 2.0 extension before using it, so what v1.3
# lacks (steal-time, PMU snapshots) is found missing, not called.
sed -i 's/^#define SBI_ECALL_VERSION_MAJOR\t\t[0-9]*$/#define SBI_ECALL_VERSION_MAJOR\t\t2/' \
    "$SBI/include/sbi/sbi_ecall.h"
grep -qP 'SBI_ECALL_VERSION_MAJOR\t\t2$' "$SBI/include/sbi/sbi_ecall.h" \
    || { echo "could not set OpenSBI's reported SBI version" >&2; exit 1; }
# Each hart the device tree names then costs OpenSBI ~18KB after its image
# (stack and scratch, and its share of the heap): 72MB for 4096 harts, so the
# kernel cannot sit at the default 2MB. 128MB leaves room; the FDT follows the
# kernel. These must match KERNEL_OFFSET and DTB_OFFSET in src/doom_system.cpp. A clean build: the
# addresses are compiler flags, which OpenSBI's dependencies do not track.
rm -rf "$SBI/build"
make -C "$SBI" PLATFORM=generic CROSS_COMPILE=riscv64-linux-gnu- \
    CC='riscv64-linux-gnu-gcc -std=gnu11' PLATFORM_RISCV_XLEN=64 \
    PLATFORM_RISCV_ISA=rv64imafdc_zicsr_zifencei PLATFORM_RISCV_ABI=lp64d \
    FW_JUMP_ADDR=0x88000000 FW_JUMP_FDT_ADDR=0x8C000000 -j"$JOBS"
cp "$SBI/build/platform/generic/firmware/fw_jump.elf" "$OUT/fw_jump.elf"

echo '=== Linux (pinned gitlink; source stays on the WSL filesystem) ==='
KERNEL="$(pinned_source linux tools/linux/linux/src https://github.com/torvalds/linux.git)"
make -C "$KERNEL" ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig
# VIRTIO_INPUT + INPUT_EVDEV: the keyboard and mouse. The framebuffer
# console above is only half a console without them -- fbcon draws on
# tty0, and the VT layer takes its keystrokes from an input device, not
# from the serial port. Without VIRTIO_INPUT the window shows a login
# prompt that cannot be typed at. EVDEV is for everything in userspace
# that wants a pointer, which reads /dev/input/event* rather than asking
# the VT layer anything.
# MAGIC_SYSRQ: riscv defconfig leaves it off, and `echo o >
# /proc/sysrq-trigger` is the only way a guest running something other than
# systemd as PID 1 can power the machine off. Ubuntu's stage-2 build is
# exactly that case: /sbin/poweroff there is systemd's and wants to talk to a
# running systemd, so without sysrq the script reaches the end of a four-hour
# build and then sits in a sleep loop instead of ending the run. It is the
# kernel, not userspace, that then calls SBI SRST and hits DoomV's
# sifive,test0 device.
# FB + FB_SIMPLE + FRAMEBUFFER_CONSOLE: the graphical console. riscv
# defconfig builds the fbdev core but no framebuffer *driver*, so the
# simple-framebuffer node in tools/linux/dts/doomv.dts has nothing to bind
# to and the kernel settles for "Console: colour dummy device 80x25" -- a
# console that exists and displays nowhere. These three give fbcon a real
# device, which is what puts Linux in the DoomV window.
# NR_CPUS: 4096, as many as the machine can have -- DoomV's CLINT serves
# harts 0..4094, and OpenSBI is built for 4096 above. arch/riscv/Kconfig stops
# at 512, so the build's copy of it is widened; the generic kernel goes to
# 8192 (x86's MAXSMP). Per-CPU data is allocated for the CPUs the device tree
# names, so a one-hart boot does not pay for the rest. NR_CPUS above 64 needs
# RISCV_SBI_V01 off, and the console then goes through SBI DBCN, which is why
# OpenSBI reports spec 2.0 (see the OpenSBI step above, and docs/BUGS.md
# bugs 18 and 172).
sed -i 's/^	range 2 [0-9]* if !RISCV_SBI_V01$/	range 2 4096 if !RISCV_SBI_V01/' "$KERNEL/arch/riscv/Kconfig"
grep -qP '^	range 2 4096 if !RISCV_SBI_V01$' "$KERNEL/arch/riscv/Kconfig"     || { echo "could not widen NR_CPUS in arch/riscv/Kconfig" >&2; exit 1; }
"$KERNEL/scripts/config" --file "$KERNEL/.config" --disable RISCV_SBI_V01 \
    --enable NONPORTABLE --enable HVC_RISCV_SBI --enable BLK_DEV_INITRD --enable BINFMT_SCRIPT \
    --enable FB --enable FB_SIMPLE --enable FRAMEBUFFER_CONSOLE \
    --enable MAGIC_SYSRQ --enable VIRTIO_INPUT --enable INPUT_EVDEV --enable SND_VIRTIO \
    --set-val NR_CPUS 4096
make -C "$KERNEL" ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
make -C "$KERNEL" ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- Image -j"$JOBS"
cp "$KERNEL/arch/riscv/boot/Image" "$OUT/Image"

echo '=== BusyBox (static userspace) ==='
BUSYBOX="$(pinned_source busybox tools/linux/rootfs/src https://github.com/mirror/busybox.git)"
make -C "$BUSYBOX" ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/; s/^CONFIG_TC=y/# CONFIG_TC is not set/' "$BUSYBOX/.config"
make -C "$BUSYBOX" ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- oldconfig < /dev/null
make -C "$BUSYBOX" ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j"$JOBS"

# Fresh staging avoids stale files from older BusyBox configurations.
STAGE="$(mktemp -d "$CACHE/rootfs.XXXXXX")"
trap 'rm -rf -- "$STAGE"' EXIT
make -C "$BUSYBOX" ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- CONFIG_PREFIX="$STAGE" install
mkdir -p "$STAGE/dev" "$STAGE/proc" "$STAGE/sys" "$STAGE/tmp"
mknod -m 600 "$STAGE/dev/console" c 5 1
mknod -m 666 "$STAGE/dev/null" c 1 3
# With -net, `udhcpc -i eth0` configures the network card: BusyBox's own
# example script sets the address, the route and /etc/resolv.conf.
mkdir -p "$STAGE/etc" "$STAGE/usr/share/udhcpc"
install -m 755 "$BUSYBOX/examples/udhcp/simple.script" "$STAGE/usr/share/udhcpc/default.script"
cp "$ROOT/scripts/linux_smoke_init.sh" "$STAGE/doomv-smoke"
chmod 755 "$STAGE/doomv-smoke"
(cd "$STAGE" && find . -print0 | sort -z | cpio --null -o --format=newc --owner=0:0) > "$OUT/smoke.cpio"
# The normal image must be packed after the smoke-only file is removed; this
# keeps the two initrds independent and makes the default boot unchanged.
rm -f "$STAGE/doomv-smoke"
(cd "$STAGE" && find . -print0 | sort -z | cpio --null -o --format=newc --owner=0:0) > "$OUT/initramfs.cpio"

python3 "$ROOT/scripts/prepare_dtb.py" "$OUT"
echo "Linux images ready in $OUT"
