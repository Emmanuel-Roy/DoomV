#!/usr/bin/env bash
# Stage 1 of building an Ubuntu riscv64 root filesystem for DoomV: unpack the
# .debs into a GPT-partitioned disk image. Nothing riscv64 is executed here.
#
# Stage 2 -- configuring those packages, which means running riscv64 dpkg,
# perl and shell -- is done by DoomV itself. See boot-stage2.sh.
#
# The usual way to cross-bootstrap is qemu-user-static plus binfmt, letting
# the host execute the target's binaries. That is deliberately not what this
# does, and not because qemu is unavailable: the point of an emulator that
# boots Linux is that it can run the distribution's own tooling. If DoomV can
# configure a hundred Ubuntu packages -- dpkg, its maintainer scripts, the
# perl they invoke, the shell those fork -- then it is running real riscv64
# userspace under real load, which is a far stronger statement than any test
# suite makes. And if it cannot, that is a bug worth finding.
#
# This is also the distribution counterpart to ../rootfs/, which builds a
# BusyBox initramfs: BusyBox is one static binary exec'd as PID 1 and asks the
# kernel for very little, while Ubuntu is systemd plus a package database plus
# a hundred services. A machine that boots BusyBox is not thereby known to
# boot a distribution.
#
# Runs inside WSL as root -- it needs loop devices, mkfs.ext4 and sgdisk,
# none of which exist on the MSYS2 side:
#
#   wsl -d Ubuntu -u root -- bash /mnt/z/Code/Dev/DoomV/tools/linux/ubuntu/mkrootfs.sh
#
# It downloads packages from ports.ubuntu.com (riscv64 is a *ports* release,
# not on archive.ubuntu.com), so it needs network and takes a while.
set -e

OUT="${1:-/mnt/z/Code/Dev/DoomV/ubuntu.img}"
SIZE_MIB="${2:-4096}"
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
# debootstrap's own dependencies, checked here because it does not check them
# itself: a missing one surfaces as a bare exit 127 from inside the unpack
# loop, with no indication of which tool was not found. zstd is the one that
# actually bites -- noble compresses .deb payloads with it, and a WSL install
# does not ship it.
for t in wget ar gpgv xz zstd; do
	command -v "$t" >/dev/null 2>&1 || {
		echo "error: $t not found -- debootstrap needs it to unpack .debs." >&2
		echo "       apt-get install -y wget binutils gpgv xz-utils zstd" >&2
		exit 1
	}
done

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

# And the other half of the same problem: git on Windows cannot create
# symlinks without developer mode, so it checks each one out as a one-line
# text file containing the target name. 47 of debootstrap-s 70 suite
# scripts are symlinks -- scripts/noble is the five bytes "gutsy" -- and
# sourcing one runs its target name as a command:
#
#   scripts/noble: gutsy: not found
#
# which debootstrap reports by exiting 127 with no message at all, having
# already redirected its own output. Materialise them by following the
# chain until a real script is reached: command substitution strips
# trailing newlines, so a smuggled symlink has no interior newline and a
# real script has many, which tells the two apart without guessing sizes.
for f in "$DEBOOTSTRAP_DIR"/scripts/*; do
	[ -f "$f" ] || continue
	hops=0
	while t="$(cat "$f")"; [ -n "$t" ] \
	      && [ "$(printf %s "$t" | wc -l)" -eq 0 ] \
	      && [ -f "$DEBOOTSTRAP_DIR/scripts/$t" ] \
	      && [ "$hops" -lt 8 ]; do
		cp "$DEBOOTSTRAP_DIR/scripts/$t" "$f"
		hops=$((hops + 1))
	done
done

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
# --variant=minbase, because every package unpacked here is a package
# DoomV has to *configure* in stage 2, at around 13 MIPS. minbase drops the
# "standard" priority set and leaves the essential ones; systemd is then
# added back explicitly, since it is the whole point of using a distribution
# rather than BusyBox. ifupdown and iproute2 are deliberately absent: there
# is no NIC in this machine, so they would be packages configured slowly to
# manage hardware that does not exist.
PACKAGES="${PACKAGES:-systemd,systemd-sysv,udev,nano,less}"
DEBOOTSTRAP_DIR="$DEBOOTSTRAP_DIR" "$DEBOOTSTRAP_DIR/debootstrap" \
	--arch=riscv64 --foreign --variant=minbase \
	--include="$PACKAGES" \
	"$SUITE" "$W/mnt" "$MIRROR"

echo "== writing the stage-2 script DoomV will run as init"
# This is PID 1 for exactly one boot, so it cannot assume much: nothing is
# mounted, no service manager exists, and if it exits the kernel panics on
# "init died". It mounts what dpkg needs, does the work, and powers the
# machine off through SBI SRST rather than returning.
cat > "$W/mnt/doomv-stage2" <<'STAGE2'
#!/bin/sh
# Ubuntu's second-stage configuration, run by DoomV as PID 1. Every binary
# executed from here on is riscv64, interpreted by the emulator.
set -x
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null || true

echo "=== DOOMV-STAGE2-BEGIN ==="
/debootstrap/debootstrap --second-stage
rc=$?
echo "=== DOOMV-STAGE2-SECOND-STAGE rc=$rc ==="

if [ "$rc" = 0 ]; then
	# Only worth configuring a system that finished unpacking.
	echo "root:doomv" | chpasswd
	echo doomv > /etc/hostname
	printf 'LABEL=ubunturoot / ext4 defaults,noatime 0 1\n' > /etc/fstab
	# DoomV's console is the SBI console, which Linux exposes as hvc0; once
	# the framebuffer binds there is a tty1 as well. Both have to be in
	# securetty for a root login to be permitted on them.
	for t in hvc0 tty1; do
		grep -qx "$t" /etc/securetty 2>/dev/null || echo "$t" >> /etc/securetty
	done
	# No NIC in this machine, so this unit would block the boot for two
	# minutes waiting for a link that never comes up.
	systemctl disable systemd-networkd-wait-online.service 2>/dev/null || true
	rm -f /doomv-stage2
	echo "=== DOOMV-STAGE2-OK ==="
else
	echo "=== DOOMV-STAGE2-FAILED ==="
fi

sync
umount /proc /sys 2>/dev/null || true
# SBI SRST, through the sifive,test0 device in the device tree. Without a way
# for the guest to end the run, an unattended build sits at a dead prompt.
poweroff -f 2>/dev/null || true
echo "=== DOOMV-STAGE2-POWEROFF-FAILED ==="
while true; do sleep 60; done
STAGE2
chmod 755 "$W/mnt/doomv-stage2"
sync

umount "$W/mnt"
losetup -d "$LOOP"; LOOP=""
cp "$W/disk.img" "$OUT"
echo
echo "wrote $OUT ($(du -h "$OUT" | cut -f1))"
echo
echo "Stage 1 done -- unpacked, not yet configured. Run stage 2 on DoomV:"
echo
echo "  tools/linux/ubuntu/boot-stage2.sh $OUT"
echo
echo "That boots the image with init=/doomv-stage2 and lets the emulator run"
echo "riscv64 dpkg against it. Expect it to take a long while."
