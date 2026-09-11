#!/usr/bin/env bash
# Boots a configured Ubuntu image with systemd as PID 1, in a window.
#
# The window is the point here, unlike the test suites: with FB_SIMPLE in the
# kernel and the framebuffer@50000000 node in the device tree, fbcon puts a
# 1024x768 graphical console into DoomV's SDL window, and that is Ubuntu being
# displayed by the emulator rather than merely logged by it. Pass -ng if you
# want the log instead.
#
# Run from MSYS2/Git Bash. Log in as root / doomv.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
IMG="${1:-$ROOT/ubuntu.img}"
shift || true
DTB="$HERE/ubuntu.dtb"

[ -f "$IMG" ] || { echo "error: $IMG not found -- run mkrootfs.sh and boot-stage2.sh." >&2; exit 1; }
[ -x "$ROOT/riscv_doom.exe" ] || { echo "error: build riscv_doom.exe first (make)." >&2; exit 1; }

# console=tty0 before console=hvc0, and the order is load-bearing: both are
# registered so the window and the serial log both get output, but the *last*
# one becomes /dev/console. hvc0 last puts the shell on the serial where a
# script can reach it; tty0 last silences the log the moment a real console
# registers, which looks exactly like a hang. This file had it backwards.
#
# Same three differences from the stock DTB as the stage-2 one, minus the init
# override: root is the partition, there is no initrd, and systemd is PID 1.
# An initrd is what you must not leave in -- the kernel would run that and
# never mount the disk, which looks like a successful boot of nothing.
python3 - "$ROOT/tools/linux/dts/doomv.dts" "$HERE/ubuntu.dts" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
src = re.sub(r'bootargs = "[^"]*";',
             'bootargs = "earlycon=sbi console=tty0 console=hvc0 '
             'root=/dev/vda1 rootwait rw";', src, count=1)
src = re.sub(r'\s*linux,initrd-(start|end)\s*=\s*<[^>]*>;', '', src)
open(sys.argv[2], "w", newline="\n").write(src)
PY
wsl_path() { printf '/mnt%s' "$1"; }
MSYS2_ARG_CONV_EXCL='*' wsl -d Ubuntu -u root -- dtc -I dts -O dtb \
	-o "$(wsl_path "$DTB")" "$(wsl_path "$HERE/ubuntu.dts")" 2>&1 | tail -2
[ -s "$DTB" ] || { echo "error: dtc produced no $DTB" >&2; exit 1; }

exec "$ROOT/riscv_doom.exe" \
	-opensbi="$ROOT/tools/linux/opensbi/fw_jump.elf" \
	-kernel="$ROOT/tools/linux/linux/Image" \
	-dtb="$DTB" -disk="$IMG" "$@"
