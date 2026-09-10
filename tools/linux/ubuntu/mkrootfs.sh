#!/usr/bin/env bash
# Builds a bootable Ubuntu riscv64 root filesystem as a GPT-partitioned disk
# image for -disk=, using the debootstrap pinned in src/.
#
# This is the distribution counterpart to ../rootfs/, which builds a BusyBox
# initramfs. The difference is not size -- it is that BusyBox is one static
# binary exec'd as PID 1, and Ubuntu is systemd plus a package database plus
# a hundred services, so it exercises cgroups, epoll, signalfd, timerfd,
# /proc, /sys, the RTC and a great deal more of the kernel than `rdinit=/bin/sh`
# ever reaches. A machine that boots BusyBox is not thereby known to boot a
# distribution.
#
# Runs inside WSL as root -- it needs loop devices, mkfs.ext4, sgdisk and
# binfmt, none of which exist on the MSYS2 side:
#
#   wsl -d Ubuntu -u root -- bash /mnt/z/Code/Dev/DoomV/tools/linux/ubuntu/mkrootfs.sh
#
# It downloads packages from ports.ubuntu.com (riscv64 is a *ports* release,
# not on archive.ubuntu.com), so it needs network and takes a while.
set -e

OUT="${1:-/mnt/z/Code/Dev/DoomV/ubuntu.img}"
SIZE_MIB="${2:-3072}"
SUITE="${SUITE:-noble}"
MIRROR="${MIRROR:-http://ports.ubuntu.com/ubuntu-ports}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/src"

[ -f "$SRC/debootstrap" ] || {
	echo "error: $SRC/debootstrap missing." >&2
	echo "       git submodule update --init tools/linux/ubuntu/src" >&2
	exit 1
}
[ -f "$SRC/scripts/$SUITE" ] || {
	echo "error: no debootstrap script for suite '$SUITE' in the pinned tree." >&2
	echo "       Available: $(ls "$SRC/scripts" | tr '\n' ' ')" >&2
	exit 1
}

for t in sgdisk mkfs.ext4 losetup; do
	command -v "$t" >/dev/null 2>&1 || { echo "error: $t not found (apt-get install gdisk e2fsprogs util-linux)" >&2; exit 1; }
done

# The second stage has to execute riscv64 binaries. binfmt_misc plus
# qemu-user-static is the ordinary way; without it debootstrap's stage 1
# still unpacks correctly and stage 2 fails on the first maintainer script,
# which is a confusing place to discover the dependency.
QEMU=""
for c in /usr/bin/qemu-riscv64-static /usr/bin/qemu-riscv64; do
	[ -x "$c" ] && QEMU="$c" && break
done
[ -n "$QEMU" ] || {
	echo "error: no qemu-riscv64 binary found -- the second stage cannot run." >&2
	echo "       apt-get install -y qemu-user-static binfmt-support" >&2
	echo "       (and check /proc/sys/fs/binfmt_misc/qemu-riscv64 appears)" >&2
	exit 1
}

W="$(mktemp -d)"
LOOP=""

# debootstrap is POSIX shell, and this repo is checked out on Windows with
# core.autocrlf=true. The parent .gitattributes deliberately leaves the
# vendored trees under tools/*/src alone -- they keep whatever upstream
# uses -- and upstream debootstrap ships no .gitattributes of its own, so a
# plain clone here arrives with CRLF endings and dies as
#
#   debootstrap: line 2: set: -: invalid option
#   debootstrap: line 69: syntax error near unexpected token
#
# which is a bewildering thing to hit inside a third-party script. Rather
# than requiring every checkout to be configured correctly, work from a
# normalised copy: the tree is a few hundred KB of text, so this costs
# nothing and cannot be forgotten.
DEBOOTSTRAP_DIR="$W/debootstrap-src"
cp -r "$SRC" "$DEBOOTSTRAP_DIR"
find "$DEBOOTSTRAP_DIR" -type f -exec sed -i 's/\r$//' {} +
chmod +x "$DEBOOTSTRAP_DIR/debootstrap"

cleanup() {
	mountpoint -q "$W/mnt/proc" && umount "$W/mnt/proc" || true
	mountpoint -q "$W/mnt/sys"  && umount "$W/mnt/sys"  || true
	mountpoint -q "$W/mnt/dev"  && umount "$W/mnt/dev"  || true
	mountpoint -q "$W/mnt"      && umount "$W/mnt"      || true
	[ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
	rm -rf "$W"
}
trap cleanup EXIT

echo "== image: ${SIZE_MIB} MiB, one GPT partition"
dd if=/dev/zero of="$W/disk.img" bs=1M count="$SIZE_MIB" status=none
sgdisk -n 1:2048:0 -t 1:8300 "$W/disk.img" >/dev/null
# Read the extent back rather than computing it -- see ../rootfs/mkdisk.sh
# for what going five blocks over costs.
START=$(sgdisk -i 1 "$W/disk.img" | grep -oP 'First sector: \K[0-9]+')
END=$(sgdisk -i 1 "$W/disk.img" | grep -oP 'Last sector: \K[0-9]+')
SECTORS=$((END - START + 1))
LOOP=$(losetup --find --show --offset $((START * 512)) --sizelimit $((SECTORS * 512)) "$W/disk.img")
mkfs.ext4 -q -F -L ubunturoot "$LOOP"
mkdir -p "$W/mnt"
mount "$LOOP" "$W/mnt"

echo "== debootstrap stage 1 ($SUITE/riscv64 from $MIRROR)"
# --foreign: unpack only. Nothing riscv64 is executed on the host in this
# stage, which is what makes cross-architecture bootstrapping possible at all.
DEBOOTSTRAP_DIR="$DEBOOTSTRAP_DIR" "$DEBOOTSTRAP_DIR/debootstrap" \
	--arch=riscv64 --foreign \
	--include=systemd,systemd-sysv,udev,init,ifupdown,iproute2,nano,less \
	"$SUITE" "$W/mnt" "$MIRROR"

echo "== debootstrap stage 2 (riscv64 via $QEMU)"
cp "$QEMU" "$W/mnt/usr/bin/$(basename "$QEMU")"
mount --bind /proc "$W/mnt/proc"
mount --bind /sys  "$W/mnt/sys"
mount --bind /dev  "$W/mnt/dev"
chroot "$W/mnt" /debootstrap/debootstrap --second-stage
umount "$W/mnt/dev" "$W/mnt/sys" "$W/mnt/proc"

echo "== configuring for DoomV"
# Root password. A machine with no console login is a machine you cannot
# check anything on, and this image never leaves the developer's disk.
chroot "$W/mnt" /bin/bash -c 'echo "root:doomv" | chpasswd'
echo doomv > "$W/mnt/etc/hostname"
cat > "$W/mnt/etc/fstab" <<'FSTAB'
# DoomV: one virtio-blk device, one partition, mounted by the kernel from
# root= on the command line. Listed here so remounts and fsck work.
LABEL=ubunturoot  /  ext4  defaults,noatime  0 1
FSTAB
# DoomV's console is the SBI console, which Linux exposes as hvc0.
# console=hvc0 on the command line is enough for systemd to start a getty
# on it by itself (serial-getty@hvc0), so no unit file is needed here --
# but the device has to be in securetty for root to be allowed to log in.
grep -qx hvc0 "$W/mnt/etc/securetty" 2>/dev/null || echo hvc0 >> "$W/mnt/etc/securetty"
# Nothing in this machine provides a network, and systemd-networkd-wait-online
# blocks the boot for two minutes waiting for one.
chroot "$W/mnt" systemctl disable systemd-networkd-wait-online.service 2>/dev/null || true
rm -f "$W/mnt/usr/bin/$(basename "$QEMU")"
sync

umount "$W/mnt"
losetup -d "$LOOP"; LOOP=""
cp "$W/disk.img" "$OUT"
echo
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
echo
echo "Boot it with a DTB whose bootargs name the partition:"
echo
echo "  sed 's|rdinit=/bin/sh|root=/dev/vda1 rootwait rw|' \\"
echo "      tools/linux/dts/doomv.dts > /tmp/ubuntu.dts"
echo "  dtc -I dts -O dtb -o ubuntu.dtb /tmp/ubuntu.dts"
echo "  riscv_doom.exe -opensbi=... -kernel=... -dtb=ubuntu.dtb -disk=$OUT"
echo
echo "No -initrd, and no init= override: systemd has to be PID 1 or none of"
echo "what makes this different from the BusyBox image happens. Log in as"
echo "root / doomv."
