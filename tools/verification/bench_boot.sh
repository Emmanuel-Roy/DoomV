#!/usr/bin/env bash
# Times a Linux boot to a fixed point, as a throughput benchmark.
#
# A kernel boot is the right workload to measure this machine with. It runs
# tens of billions of instructions, it turns paging on early and then does
# every access through a three-level Sv39 walk, and it is exactly what the
# slow parts of real use look like -- unlike a tight bare-metal loop, which
# measures the decoder and nothing else.
#
# Reports wall seconds and the retired-instruction rate to the marker. Run it
# before and after a change, on an otherwise-quiet machine: the number is
# only meaningful against another number taken the same way.
#
#   tools/verification/bench_boot.sh                 # default binary
#   tools/verification/bench_boot.sh riscv_doom_dev.exe
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-riscv_doom.exe}"
# Accept a bare name, a relative path or an absolute one. A bare name has to
# be resolved against the repo root rather than left for the shell, which
# would look it up on PATH and not find it.
case "$BIN" in
	/*) ;;
	*/*) BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" ;;
	*)  BIN="$ROOT/$BIN" ;;
esac
MARKER="${MARKER:-Run /bin/sh as init process}"
LOG="$(mktemp -t doomv-bench.XXXXXX)"

IMAGES="$ROOT/build/linux"
[ -f "$IMAGES/Image" ] || IMAGES="$ROOT/tools/linux"
KERNEL="$IMAGES/Image"; [ -f "$KERNEL" ] || KERNEL="$ROOT/tools/linux/linux/Image"
SBI="$IMAGES/fw_jump.elf"; [ -f "$SBI" ] || SBI="$ROOT/tools/linux/opensbi/fw_jump.elf"
DTB="$ROOT/tools/linux/dts/doomv.dtb"
INITRD="$ROOT/tools/linux/rootfs/initramfs.cpio"
for f in "$BIN" "$KERNEL" "$SBI" "$DTB" "$INITRD"; do
	[ -f "$f" ] || { echo "error: $f missing" >&2; exit 1; }
done

echo "binary: $BIN"
start=$(date +%s.%N)
"$BIN" -ng -opensbi="$SBI" -kernel="$KERNEL" -dtb="$DTB" -initrd="$INITRD" > "$LOG" 2>&1 &
pid=$!

# Poll for the marker rather than waiting on the process: the guest reaches a
# shell and then sits there, so the run never ends on its own.
for _ in $(seq 1 600); do
	grep -aq "$MARKER" "$LOG" 2>/dev/null && break
	kill -0 "$pid" 2>/dev/null || break
	sleep 1
done
end=$(date +%s.%N)
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

if ! grep -aq "$MARKER" "$LOG"; then
	echo "FAILED: never reached the marker. Last lines:"
	tail -5 "$LOG"
	rm -f "$LOG"
	exit 1
fi

# The guest's own timestamp on the marker line is retired instructions in
# disguise: mtime advances once per instruction on this machine (see the
# counters note in README.md), so the kernel's printk clock is a cycle count
# with a fixed scale. Dividing it by the wall time gives the emulated rate
# without needing the emulator to report one.
guest=$(grep -a "$MARKER" "$LOG" | head -1 | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p')
wall=$(echo "$end $start" | awk '{printf "%.2f", $1 - $2}')
echo "marker:      $MARKER"
echo "guest time:  ${guest}s"
echo "wall time:   ${wall}s"
# timebase-frequency is 1e9 in tools/linux/dts/doomv.dts and Timer::step does
# `mtime += retired`, so one guest nanosecond is one retired instruction and
# the kernel's printk timestamp converts straight into an instruction count.
awk -v g="$guest" -v w="$wall" 'BEGIN {
	if (g > 0 && w > 0) {
		insns = g * 1e9
		printf "retired:     %.0f instructions\n", insns
		printf "throughput:  %.2f MIPS\n", insns / w / 1e6
	}
}'
rm -f "$LOG"
