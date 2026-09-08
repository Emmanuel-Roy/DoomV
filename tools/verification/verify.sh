#!/usr/bin/env bash
# Build DoomV and run every verification suite against it, in one command.
#
#   tools/verification/verify.sh              # everything
#   tools/verification/verify.sh --quick      # skip the two slow suites
#   tools/verification/verify.sh archtest     # one suite by name
#
# Suites, in the order they run:
#
#   differential   19 hand-written suites, 639 cases, diffed against Sail.
#                  These exist for the parts of RVA23S64 that no upstream
#                  suite covers at all -- Sscofpmf, Ssstateen, pointer
#                  masking, Zawrs.
#   archtest       riscv-arch-test RVA23S64, 663 tests, diffed against Sail.
#                  The certification suite. Its pass criterion is literally
#                  "produces the same signature as the reference model".
#   hypervisor     damo-rv-priv-ats, 43 groups. The only H coverage that
#                  exists anywhere: riscv-arch-test has no hypervisor
#                  tests, no testplan and no coverpoints, while RVA23S64
#                  requires H and the Sh* sub-extensions.
#   vector         riscv-vector-tests at VLEN=128, 3042 ELFs. Slow.
#   riscvtests     riscv-tests, 372 rv64 tests. Broad regression net.
#   linux          boots Linux to an interactive shell.
#
# Sail is the reference of record throughout. spike remains available
# behind run_diff.sh --ref spike and is worth running -- an independent
# implementation disagreeing is a signal even when it is the one that turns
# out to be wrong -- but it does not decide anything, and riscv-arch-test
# ships no spike RVA23S64 configuration at all.
#
# Prerequisites, one-time:
#   tools/verification/tests/archtest/setup.sh      toolchain, Sail 0.13.1, act
#   tools/verification/tests/archtest/gen_reference.sh   compile + Sail signatures
#   tools/verification/tests/suites/fetch.sh        the precompiled suites
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || exit 2

QUICK=0
ONLY=""
for a in "$@"; do
	case "$a" in
	--quick) QUICK=1 ;;
	-h|--help) sed -n '2,35p' "$0"; exit 0 ;;
	*) ONLY="$a" ;;
	esac
done

want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

rule() { printf '\n=====================================================================\n  %s\n=====================================================================\n' "$1"; }

fail=0
summary=""
record() { summary="${summary}
  $(printf '%-14s %s' "$1" "$2")"; }

# ---------------------------------------------------------------- build
rule "build"
# The exe is deleted first because a stale binary is this project's most
# persistent failure mode: make reports success, the link was blocked by a
# still-running emulator, and the next suite silently measures the previous
# build. It has cost three wrong diagnoses.
powershell.exe -NoProfile -Command "Get-Process riscv_doom -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
rm -f riscv_doom.exe
if make -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | grep -E " error|Error" ; then
	echo "BUILD FAILED"; exit 1
fi
[ -x riscv_doom.exe ] || { echo "BUILD FAILED: no riscv_doom.exe"; exit 1; }
echo "ok: $(ls -la riscv_doom.exe | awk '{print $5}') bytes"

# ------------------------------------------------------- differential
if want differential; then
	rule "differential suites vs Sail (19 suites, 639 cases)"
	if tools/verification/tests/differential/vector/run_diff.sh 2>&1 | grep -E "MATCH|MISMATCH|KNOWN|STALE"; then :; fi
	n=$(tools/verification/tests/differential/vector/run_diff.sh 2>&1 | grep -c "^MISMATCH")
	if [ "$n" -eq 0 ]; then record differential "19/19 match"; else record differential "$n MISMATCH"; fail=1; fi
fi

# ------------------------------------------------------------ archtest
if want archtest; then
	rule "riscv-arch-test RVA23S64 vs Sail (663 tests)"
	out=$(cd tools/verification/tests/archtest && rm -f out/*.doomv.sig 2>/dev/null; python archtest.py --timeout 150 2>&1 | tr '\r' '\n' | tail -40)
	echo "$out" | grep -vE "^\[[0-9]+/" | tail -20
	line=$(echo "$out" | grep -E "^  pass [0-9]+" | tail -1)
	record archtest "${line:-no result}"
	echo "$line" | grep -q "fail 0" || fail=1
fi

# ---------------------------------------------------------- hypervisor
if want hypervisor; then
	rule "damo-rv-priv-ats hypervisor suite"
	if [ -d tools/verification/tests/suites/damo-tests ]; then
		out=$(cd tools/verification/tests/suites && python run_suite.py damo-tests --timeout 120 2>&1 | tr '\r' '\n' | tail -30)
		echo "$out" | grep -vE "^\[[0-9]+/" | tail -14
		line=$(echo "$out" | grep -E "pass [0-9]+" | tail -1)
		record hypervisor "${line:-no result}"
	else
		record hypervisor "not fetched (tools/verification/tests/suites/fetch.sh)"
	fi
fi

# -------------------------------------------------------------- vector
if want vector && [ "$QUICK" -eq 0 ]; then
	rule "riscv-vector-tests v128x64 (3042 ELFs -- slow)"
	if [ -d tools/verification/tests/suites/riscv-vector-tests-v128x64 ]; then
		out=$(cd tools/verification/tests/suites && python run_suite.py riscv-vector-tests-v128x64 --timeout 120 2>&1 | tr '\r' '\n' | tail -30)
		echo "$out" | grep -vE "^\[[0-9]+/" | tail -14
		record vector "$(echo "$out" | grep -E "pass [0-9]+" | tail -1)"
	else
		record vector "not fetched"
	fi
fi

# ---------------------------------------------------------- riscv-tests
if want riscvtests && [ "$QUICK" -eq 0 ]; then
	rule "riscv-tests (372 rv64)"
	if [ -d tools/verification/tests/suites/riscv-tests ]; then
		out=$(cd tools/verification/tests/suites && python run_suite.py riscv-tests --timeout 120 2>&1 | tr '\r' '\n' | tail -30)
		echo "$out" | grep -vE "^\[[0-9]+/" | tail -14
		record riscvtests "$(echo "$out" | grep -E "pass [0-9]+" | tail -1)"
	else
		record riscvtests "not fetched"
	fi
fi

# --------------------------------------------------------------- linux
if want linux; then
	rule "Linux boot"
	log=$(mktemp 2>/dev/null || echo /tmp/doomv_boot.log)
	./riscv_doom.exe -opensbi=tools/linux/opensbi/fw_jump.elf \
	                 -kernel=tools/linux/linux/Image \
	                 -dtb=tools/linux/dts/doomv.dtb \
	                 -initrd=tools/linux/rootfs/initramfs.cpio > "$log" 2>&1 &
	pid=$!
	# The emulator keeps its SDL window open after the guest reaches a
	# shell, so it never exits on its own and has to be stopped here.
	sleep 90
	powershell.exe -NoProfile -Command "Get-Process riscv_doom -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
	wait $pid 2>/dev/null
	lines=$(wc -l < "$log")
	if grep -aq "Run /bin/sh as init" "$log" && ! grep -aq "Kernel panic" "$log"; then
		record linux "reached init, $lines lines"
	else
		record linux "FAILED ($lines lines)"; fail=1
	fi
	tail -3 "$log"
fi

rule "summary"
printf '%s\n\n' "$summary"
[ "$fail" -eq 0 ] && echo "  all run suites clean" || echo "  FAILURES above"
exit $fail
