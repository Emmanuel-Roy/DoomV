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
make -C "$SBI" PLATFORM=generic CROSS_COMPILE=riscv64-linux-gnu- \
    CC='riscv64-linux-gnu-gcc -std=gnu11' PLATFORM_RISCV_XLEN=64 \
    PLATFORM_RISCV_ISA=rv64imafdc_zicsr_zifencei PLATFORM_RISCV_ABI=lp64d -j"$JOBS"
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
"$KERNEL/scripts/config" --file "$KERNEL/.config" --enable RISCV_SBI_V01 \
    --enable NONPORTABLE --enable HVC_RISCV_SBI --enable BLK_DEV_INITRD --enable BINFMT_SCRIPT \
    --enable FB --enable FB_SIMPLE --enable FRAMEBUFFER_CONSOLE \
    --enable MAGIC_SYSRQ --enable VIRTIO_INPUT --enable INPUT_EVDEV
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
cp "$ROOT/scripts/linux_smoke_init.sh" "$STAGE/doomv-smoke"
chmod 755 "$STAGE/doomv-smoke"
(cd "$STAGE" && find . -print0 | sort -z | cpio --null -o --format=newc --owner=0:0) > "$OUT/smoke.cpio"
# The normal image must be packed after the smoke-only file is removed; this
# keeps the two initrds independent and makes the default boot unchanged.
rm -f "$STAGE/doomv-smoke"
(cd "$STAGE" && find . -print0 | sort -z | cpio --null -o --format=newc --owner=0:0) > "$OUT/initramfs.cpio"

python3 "$ROOT/scripts/prepare_dtb.py" "$OUT"
echo "Linux images ready in $OUT"
