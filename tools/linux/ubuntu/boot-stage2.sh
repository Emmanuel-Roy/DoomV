#!/usr/bin/env bash
# Stage 2 of the Ubuntu build: run it on DoomV.
#
# mkrootfs.sh unpacked the .debs without executing anything riscv64. This
# boots that image with init=/doomv-stage2, so the emulator runs Ubuntu's own
# dpkg -- and the maintainer scripts, and the perl and shell they fork -- to
# configure the system. That is the whole point: not "an emulator that passes
# tests" but one that runs a distribution's own tooling against itself.
#
# Runs from MSYS2/Git Bash on the Windows side, unlike mkrootfs.sh. It needs
# riscv_doom.exe and dtc; dtc comes from WSL.
#
# Expect this to take a long time. DoomV runs around 13 MIPS, and configuring
# a hundred packages is a great deal of dpkg. The guest powers itself off
# through SBI SRST when it finishes, so this is unattended -- watch the log.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
IMG="${1:-$ROOT/ubuntu.img}"
LOG="${2:-$ROOT/ubuntu-stage2.log}"
DTB="$HERE/stage2.dtb"

[ -f "$IMG" ] || { echo "error: $IMG not found -- run mkrootfs.sh first." >&2; exit 1; }
[ -x "$ROOT/riscv_doom.exe" ] || { echo "error: build riscv_doom.exe first (make)." >&2; exit 1; }
for f in "$ROOT/tools/linux/opensbi/fw_jump.elf" "$ROOT/tools/linux/linux/Image"; do
	[ -f "$f" ] || { echo "error: $f missing -- see scripts/build_linux.sh." >&2; exit 1; }
done

# A DTB of its own, because the bootargs differ from every other boot here in
# three ways that all matter:
#
#   root=/dev/vda1  -- the partition, not the whole device
#   init=/doomv-stage2  -- not systemd; systemd cannot start on an unconfigured
#                          system, and this is the one boot where that is true
#   no initrd  -- with one present the kernel runs that instead and never
#                 touches the disk, which looks like success and does nothing
#
# The initrd properties are dropped rather than left pointing at a stale
# address: a /chosen with linux,initrd-start set and no initramfs loaded is
# how you get a kernel that unpacks garbage and panics.
echo "== compiling $DTB"
python3 - "$ROOT/tools/linux/dts/doomv.dts" "$HERE/stage2.dts" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
src = re.sub(r'bootargs = "[^"]*";',
             'bootargs = "earlycon=sbi console=hvc0 ignore_loglevel '
             'root=/dev/vda1 rootwait rw init=/doomv-stage2";', src, count=1)
src = re.sub(r'\s*linux,initrd-(start|end)\s*=\s*<[^>]*>;', '', src)
open(sys.argv[2], "w", newline="\n").write(src)
PY
# MSYS2 spells the drive as /z/..., WSL as /mnt/z/... -- one prefix apart.
wsl_path() { printf '/mnt%s' "$1"; }
MSYS2_ARG_CONV_EXCL='*' wsl -d Ubuntu -u root -- dtc -I dts -O dtb \
	-o "$(wsl_path "$DTB")" "$(wsl_path "$HERE/stage2.dts")" 2>&1 | tail -2
[ -s "$DTB" ] || { echo "error: dtc produced no $DTB" >&2; exit 1; }

echo "== booting DoomV to configure the image (this is the slow part)"
echo "   log: $LOG"
# -ng: no window. There is nothing to look at -- the interesting output is
# dpkg's, and it comes out of the console into the log.
"$ROOT/riscv_doom.exe" -ng \
	-opensbi="$ROOT/tools/linux/opensbi/fw_jump.elf" \
	-kernel="$ROOT/tools/linux/linux/Image" \
	-dtb="$DTB" -disk="$IMG" > "$LOG" 2>&1 || true

echo
if grep -q "DOOMV-STAGE2-OK" "$LOG"; then
	echo "stage 2 complete -- $IMG is a configured Ubuntu system."
	echo "Boot it with systemd as PID 1:"
	echo
	echo "  tools/linux/ubuntu/boot.sh $IMG"
elif grep -q "DOOMV-STAGE2-FAILED" "$LOG"; then
	echo "stage 2 ran and debootstrap --second-stage failed. The rc and the"
	echo "failing package are in $LOG; dpkg is verbose about which one."
	exit 1
else
	echo "stage 2 did not reach either marker -- it did not finish. Last lines:"
	tail -20 "$LOG"
	exit 1
fi
