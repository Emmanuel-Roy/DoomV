#!/usr/bin/env bash
# End-to-end differential test: builds each test, runs it under spike
# (reference) and DoomV (device under test), and diffs the signature dumps.
#
# Run from a POSIX shell on the Windows side (Git Bash). DoomV is a native
# .exe while the cross-compiler and spike live in WSL, so this drives both.
#
#   ./run_diff.sh            # all tests
#   ./run_diff.sh vtest_zb   # just one
#
# Env: DISTRO (default Ubuntu), SPIKE (passed through to spike_sig.sh).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
DISTRO="${DISTRO:-Ubuntu}"
TESTS=("${@:-vtest_v vtest_zb}")
[ $# -gt 0 ] && TESTS=("$@") || TESTS=(vtest_v vtest_zb vtest_mmu vtest_trap vtest_restart)

# Windows path -> WSL mount point (Z:\Code\... -> /mnt/z/Code/...).
win_to_wsl() {
	local p; p="$(cd "$1" && pwd -W 2>/dev/null || echo "$1")"
	local drive="${p:0:1}"
	printf '/mnt/%s%s' "$(printf '%s' "$drive" | tr 'A-Z' 'a-z')" "$(printf '%s' "${p:2}")"
}

HERE_WSL="$(win_to_wsl "$HERE")"

# MSYS2_ARG_CONV_EXCL stops Git Bash rewriting /mnt/... into a Windows path
# before wsl.exe ever sees it.
wsl_run() { MSYS2_ARG_CONV_EXCL='*' wsl -d "$DISTRO" -u root -- "$@"; }

# DoomV's -march resets every extension it does not name, so each test has to
# spell out the full set it needs, not just its own additions.
march_for() {
	case "$1" in
	vtest_v)  echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_zb) echo "rv64imafdc_zicsr_zifencei_zba_zbb_zbs_zicond" ;;
	vtest_mmu) echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_trap) echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_restart) echo "rv64imafdcv_zicsr_zifencei" ;;
	esac
}

fail=0
for t in "${TESTS[@]}"; do
	echo "=============================================================="
	echo "  $t"
	echo "=============================================================="

	wsl_run bash "$HERE_WSL/spike_sig.sh" "$t" || { echo "spike side failed"; fail=1; continue; }

	# grep/cut rather than awk: an awk program full of $1/$3 has to survive
	# two levels of shell quoting on the way to WSL, and loses.
	nmsym() {
		wsl_run bash -c "riscv64-linux-gnu-nm '$HERE_WSL/$t.elf' | grep ' $1\$' | cut -d' ' -f1"
	}
	HALT="$(nmsym 'T rvtest_halt_doomv')"
	BEG="$(nmsym 'D begin_signature')"
	END="$(nmsym 'D end_signature')"

	( cd "$ROOT" && rm -f signature.log crash.log &&
	  timeout 180 ./riscv_doom.exe tools/doombuild/DOOM1.WAD \
		"tools/vtest/vector/$t.elf" -march="$(march_for "$t")" \
		-sig="$BEG:$END" -break="0x$HALT" >/dev/null 2>&1 )

	if [ ! -f "$ROOT/signature.log" ]; then
		echo "DoomV produced no signature.log"; fail=1; continue
	fi
	cp "$ROOT/signature.log" "$HERE/$t.doomv.sig"

	python "$HERE/compare.py" "$t" || fail=1
	echo ""
done

exit $fail
