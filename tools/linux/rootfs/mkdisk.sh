#!/usr/bin/env bash
# Builds a GPT-partitioned ext4 disk image for -disk=, with a root
# filesystem the kernel mounts as /dev/vda1 rather than as the whole device.
#
# Why partitioned rather than a bare filesystem on the raw device: because
# that is the shape every distribution image has. A bare ext4 written
# straight to the device works and is what tools/linux/dts/doomv.dts's
# root=/dev/vda expects, but nothing you download looks like that, so
# testing only that shape leaves the partition path unexercised. It does
# work -- virtio-blk reports the right capacity, the kernel finds the
# primary GPT, enumerates vda1, and mounts it -- and this script exists so
# that stays true.
#
# Runs inside WSL, not MSYS2: it needs loop devices, sgdisk and mkfs.ext4,
# none of which exist on the Windows side. Invoke it as
#
#   wsl -d Ubuntu -u root -- bash /mnt/z/.../tools/linux/rootfs/mkdisk.sh [out.img] [size_mib]
#
# and pass the resulting image to DoomV with -disk=, using a DTB whose
# bootargs say root=/dev/vda1 (see the note at the bottom).
set -e

OUT="${1:-/mnt/z/Code/Dev/DoomV/rootfs.img}"
SIZE_MIB="${2:-192}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CPIO="$HERE/initramfs.cpio"

for t in sgdisk mkfs.ext4 losetup cpio; do
	command -v "$t" >/dev/null 2>&1 || { echo "error: $t not found (run this inside WSL as root)" >&2; exit 1; }
done
[ -f "$CPIO" ] || { echo "error: $CPIO missing -- build BusyBox first, see README.md" >&2; exit 1; }

W="$(mktemp -d)"
cleanup() {
	mountpoint -q "$W/mnt" && umount "$W/mnt" || true
	[ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null || true
	rm -rf "$W"
}
trap cleanup EXIT

dd if=/dev/zero of="$W/disk.img" bs=1M count="$SIZE_MIB" status=none

# One Linux-filesystem partition (type 8300) from 1MiB to the end. sgdisk
# leaves room for the backup GPT at the tail by itself.
sgdisk -n 1:2048:0 -t 1:8300 "$W/disk.img" >/dev/null

# Ask the partition table for the extent rather than computing it. Getting
# this wrong is quiet and confusing: a filesystem five 4K blocks longer
# than its partition mounts fine on the host (which reads the file, not the
# partition) and fails in the guest with "bad geometry: block count 48896
# exceeds size of device (48891 blocks)".
START=$(sgdisk -i 1 "$W/disk.img" | grep -oP 'First sector: \K[0-9]+')
END=$(sgdisk -i 1 "$W/disk.img" | grep -oP 'Last sector: \K[0-9]+')
SECTORS=$((END - START + 1))
echo "partition 1: start=$START, $SECTORS sectors ($((SECTORS / 2048)) MiB)"

LOOP=$(losetup --find --show --offset $((START * 512)) --sizelimit $((SECTORS * 512)) "$W/disk.img")
mkfs.ext4 -q -F -L doomvroot "$LOOP"
mkdir -p "$W/mnt"
mount "$LOOP" "$W/mnt"

# The same BusyBox that goes into the initramfs, so a disk boot and an
# initramfs boot are running identical userspace and any difference between
# them is the block path rather than the root filesystem's contents.
( cd "$W/mnt" && cpio -idm --quiet < "$CPIO" )
mkdir -p "$W/mnt/proc" "$W/mnt/sys" "$W/mnt/dev"
sync

umount "$W/mnt"
losetup -d "$LOOP"
LOOP=""
cp "$W/disk.img" "$OUT"
echo "wrote $OUT"
echo
echo "To boot it, the DTB's bootargs need root=/dev/vda1 -- doomv.dts ships"
echo "rdinit=/bin/sh for the initramfs, which never reaches a block device:"
echo
echo "  sed 's|rdinit=/bin/sh|root=/dev/vda1 rootwait rw init=/bin/sh|' \\"
echo "      tools/linux/dts/doomv.dts > /tmp/disk.dts"
echo "  dtc -I dts -O dtb -o disk.dtb /tmp/disk.dts"
echo "  riscv_doom.exe -ng -opensbi=... -kernel=... -dtb=disk.dtb -disk=$OUT"
echo
echo "No -initrd: with one present the kernel runs that instead and the"
echo "disk is never mounted, which looks like success and proves nothing."
