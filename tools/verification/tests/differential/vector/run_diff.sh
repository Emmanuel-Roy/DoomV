#!/usr/bin/env bash
# End-to-end differential test: builds each test, runs it under a reference
# and under DoomV (the device under test), and diffs the signature dumps.
#
# Run from a POSIX shell on the Windows side (Git Bash). DoomV is a native
# .exe while the cross-compiler and both references live in WSL, so this
# drives across the boundary.
#
#   ./run_diff.sh                       # every test, against Sail
#   ./run_diff.sh vtest_zb              # one test
#   ./run_diff.sh --ref spike           # every test, against spike
#   ./run_diff.sh --ref spike vtest_zb  # one test, against spike
#
# Two references, and they are not interchangeable in authority.
#
# Sail is the golden reference for RVA23S64, and is the default here. It is
# the formal specification -- generated from the same source the
# architecture is defined in -- rather than an independent reimplementation,
# and it is the only one riscv-arch-test ships an RVA23S64 configuration
# for. At profile level spike is not a second opinion so much as an absence
# of one.
#
# spike remains available behind --ref spike, and is still worth running: an
# independent implementation disagreeing is a signal even when it is the one
# that turns out to be wrong. But it does not decide anything. Where the two
# disagree, Sail is right unless the difference is a configuration or an
# open architectural choice -- and those must not be diffed at all. A place
# where spike departs from the architecture belongs in KNOWN_DIVERGENCES in
# compare.py, not in a change to DoomV.
#
# Running both is worth the time. Sail is what caught these tests depending
# on spike's permissive PMP default, which spike could not have revealed
# because it was the thing being depended on.
#
# Env: DISTRO (default Ubuntu), SPIKE / SAIL / SAIL_CONFIG (passed through).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# Five levels up, not three: this suite moved from tools/vtest/vector to
# tools/verification/tests/differential/vector. Getting this wrong does not
# fail loudly -- ROOT would point inside tools/ and every run would report
# a missing signature.
ROOT="$(cd "$HERE/../../../../.." && pwd)"
DISTRO="${DISTRO:-Ubuntu}"

REF=sail
if [ "${1:-}" = "--ref" ]; then
	REF="${2:-sail}"; shift 2
fi
case "$REF" in
spike|sail) ;;
*) echo "unknown reference '$REF' (expected spike or sail)" >&2; exit 2 ;;
esac

TESTS=("${@:-vtest_v vtest_zb}")
[ $# -gt 0 ] && TESTS=("$@") || TESTS=(vtest_v vtest_zb vtest_fd vtest_mmu vtest_trap vtest_restart vtest_hints vtest_csr vtest_zvbb vtest_zfa vtest_zfh vtest_zvfh vtest_sv vtest_pm vtest_h vtest_hlv vtest_hgatp vtest_hdeleg vtest_stateen)

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
	vtest_fd) echo "rv64imafdc_zicsr_zifencei" ;;
	vtest_mmu) echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_trap) echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_restart) echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_hints) echo "rv64imafdc_zicsr_zifencei_zihintpause_zihintntl_zimop_zcmop_zicbom_zicbop" ;;
	vtest_csr) echo "rv64imafdc_zicsr_zifencei_zicboz_zawrs" ;;
	vtest_zvbb) echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_zfa) echo "rv64imafdc_zicsr_zifencei_zfa" ;;
	vtest_zfh) echo "rv64imafdc_zicsr_zifencei_zfhmin" ;;
	vtest_zvfh) echo "rv64imafdcv_zicsr_zifencei" ;;
	vtest_sv) echo "rv64imafdc_zicsr_zifencei_svinval_svnapot_svpbmt" ;;
	vtest_pm) echo "rv64imafdc_zicsr_zifencei_ssnpm_smnpm" ;;
	vtest_h) echo "rv64imafdch_zicsr_zifencei_ssnpm_smnpm" ;;
	vtest_hlv) echo "rv64imafdch_zicsr_zifencei" ;;
	vtest_hgatp) echo "rv64imafdch_zicsr_zifencei" ;;
	vtest_hdeleg) echo "rv64imafdch_zicsr_zifencei" ;;
	vtest_stateen) echo "rv64imafdch_zicsr_zifencei_sscofpmf_ssstateen" ;;
	esac
}

fail=0
for t in "${TESTS[@]}"; do
	echo "=============================================================="
	echo "  $t"
	echo "=============================================================="

	# Build the ELF first, then dump the signature with whichever reference
	# was asked for. These are separate steps so that a Sail run -- the
	# default, and the golden reference for RVA23S64 -- never requires spike
	# to be installed at all.
	wsl_run bash "$HERE_WSL/build_elf.sh" "$t" || { echo "build failed"; fail=1; continue; }
	if [ "$REF" = sail ]; then
		wsl_run bash "$HERE_WSL/sail_sig.sh" "$t" || { echo "sail side failed"; fail=1; continue; }
	else
		wsl_run bash "$HERE_WSL/spike_sig.sh" "$t" || { echo "spike side failed"; fail=1; continue; }
	fi

	# grep/cut rather than awk: an awk program full of $1/$3 has to survive
	# two levels of shell quoting on the way to WSL, and loses.
	nmsym() {
		wsl_run bash -c "riscv64-linux-gnu-nm '$HERE_WSL/$t.elf' | grep ' $1\$' | cut -d' ' -f1"
	}
	HALT="$(nmsym 'T rvtest_halt_doomv')"
	BEG="$(nmsym 'D begin_signature')"
	END="$(nmsym 'D end_signature')"

	if [ -z "$HALT" ] || [ -z "$BEG" ] || [ -z "$END" ]; then
        echo "Missing ELF symbols for $t"; fail=1; continue
    fi
    python "$ROOT/scripts/run_dut.py" "$HERE/$t.elf" "$HERE/$t.doomv.sig" \
        --march="$(march_for "$t")" --begin="$BEG" --end="$END" --halt="$HALT" \
        || { fail=1; continue; }

	python "$HERE/compare.py" "$t" "$REF" || fail=1
	echo ""
done

exit $fail
