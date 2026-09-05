#!/usr/bin/env bash
# Differential-tests DoomV's V extension against spike using vtest_v.S.
#
# Run from a POSIX shell on the Windows side (Git Bash): DoomV is a native
# .exe, while the cross-compiler and spike live in WSL, so this drives both
# and diffs the two signature dumps. Set DISTRO if your WSL distro is not
# named Ubuntu, and SPIKE if spike is not at the default build path.
#
# VLEN must match on both sides or nothing is comparable: DoomV is fixed at
# 128 (Registers::VLEN_BITS) and spike is told the same via --varch.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
DISTRO="${DISTRO:-Ubuntu}"
SPIKE="${SPIKE:-/root/build/spike-build/spike}"
VLEN=128

wsl_run() { wsl -d "$DISTRO" -u root -- bash -c "$1"; }

# The repo path as WSL sees it (Z:\Code\... -> /mnt/z/Code/...).
to_wsl_path() {
	local p="$1"
	local drive="${p:0:1}"
	printf '/mnt/%s%s' "$(printf '%s' "$drive" | tr 'A-Z' 'a-z')" "$(printf '%s' "${p:2}" | tr '\\' '/')"
}

ELF_WIN="$HERE/vtest_v.elf"
ELF_WSL="$(to_wsl_path "$(cd "$HERE" && pwd -W 2>/dev/null || echo "$HERE")")/vtest_v.elf"
HERE_WSL="$(dirname "$ELF_WSL")"

echo "==> compiling vtest_v.S (WSL, riscv64-linux-gnu-gcc)"
wsl_run "cd '$HERE_WSL' && riscv64-linux-gnu-gcc -march=rv64gcv -mabi=lp64d -static \
	-mcmodel=medany -nostdlib -nostartfiles -T vtest_v.lds -o vtest_v.elf vtest_v.S" || {
	echo "compile failed"; exit 1; }

# The signature bounds and the halt address are read back from the ELF
# rather than hardcoded, so editing the test never desynchronises them.
read -r BEGIN END HALT <<<"$(wsl_run "riscv64-linux-gnu-nm '$HERE_WSL/vtest_v.elf' | awk '
	\$3==\"begin_signature\"{b=\$1} \$3==\"end_signature\"{e=\$1} \$3==\"rvtest_halt_doomv\"{h=\$1}
	END{print b, e, h}'")"
echo "    begin_signature=0x$BEGIN end_signature=0x$END halt=0x$HALT"

echo "==> running spike (reference, vlen:$VLEN)"
wsl_run "cd '$HERE_WSL' && rm -f spike.sig && '$SPIKE' --isa=rv64gcv_zicsr_zifencei \
	--varch=vlen:$VLEN,elen:64 +signature=spike.sig +signature-granularity=4 vtest_v.elf" \
	>/dev/null 2>&1
if ! wsl_run "test -s '$HERE_WSL/spike.sig'"; then
	echo "spike produced no signature -- is $SPIKE built?"; exit 1
fi

echo "==> running DoomV (device under test)"
cd "$ROOT" || exit 1
rm -f signature.log crash.log
timeout 120 ./riscv_doom.exe tools/doombuild/DOOM1.WAD "$ELF_WIN" \
	-march=rv64imafdcv_zicsr_zifencei \
	-sig="$BEGIN:$END" -break="0x$HALT" >/dev/null 2>&1
[ -f signature.log ] || { echo "DoomV produced no signature.log"; exit 1; }

# Both sides emit one 32-bit word per line, most-significant nibble first.
wsl_run "cp '$HERE_WSL/spike.sig' /tmp/spike.sig"
cp signature.log "$HERE/doomv.sig"
wsl_run "cp '$HERE_WSL/../../../signature.log' /tmp/doomv.sig 2>/dev/null" || true

echo "==> diff"
wsl_run "cd '$HERE_WSL' && diff <(tr 'A-F' 'a-f' < spike.sig | sed 's/^0*//') \
	<(tr 'A-F' 'a-f' < '$HERE_WSL/doomv.sig' | sed 's/^0*//') > /tmp/vdiff.txt 2>&1; \
	if [ -s /tmp/vdiff.txt ]; then echo 'MISMATCH'; head -40 /tmp/vdiff.txt; else echo 'MATCH -- all vector results identical'; fi"
