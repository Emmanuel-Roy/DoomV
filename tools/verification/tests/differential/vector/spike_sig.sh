#!/usr/bin/env bash
# Builds a differential test and dumps spike's signature region, in the same
# format DoomV's -sig writes: one 32-bit word per line, low word of each
# doubleword first.
#
# Usage (run inside WSL):  spike_sig.sh [test_basename]
#   vtest_v  (default) -- the V extension
#   vtest_zb           -- the scalar bitmanip families
#
# Uses spike's debug mode rather than its +signature plusarg. The plusarg is
# the documented arch-test path, but this spike build never populates
# sig_file from it (htif_t::stop() then sees an empty filename and writes
# nothing, silently, while still exiting 0). Driving `until pc` + `mem` from
# a command file is version-proof and needs no special build.
set -euo pipefail

TEST="${1:-vtest_v}"
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

NM=riscv64-linux-gnu-nm
SPIKE="${SPIKE:-/root/build/spike-build/spike}"

# The ELF build and the per-test extension sets live in build_elf.sh, so a
# Sail run does not have to invoke spike just to get an ELF. Sourcing it
# here yields MARCH/ISA and builds the ELF.
. "$DIR/build_elf.sh" "$TEST"

sym() { $NM "$TEST.elf" | awk -v s="$1" '$3==s {print $1}'; }
HALT=$(sym sig_done)
BEG=$(sym begin_signature)
END=$(sym end_signature)
echo "$TEST: halt=0x$HALT begin=0x$BEG end=0x$END" >&2

python3 - "$HALT" "$BEG" "$END" > /tmp/spike_cmds.txt <<'PY'
import sys
halt, beg, end = (int(x, 16) for x in sys.argv[1:4])
print("until pc 0 0x%x" % halt)
for a in range(beg, end, 8):
    print("mem 0x%x" % a)
print("q")
PY

# --debug-cmd is required, not just piping to stdin: spike reads its
# interactive commands from that file, and anything arriving on stdin is
# treated as an empty line, i.e. "step one instruction". Piping silently
# single-steps once per line instead of running the commands.
timeout 120 "$SPIKE" -d --debug-cmd=/tmp/spike_cmds.txt --isa="$ISA" "./$TEST.elf" \
    > /tmp/spike_raw.txt 2>&1 || true

# spike refusing to start at all -- an unusable --isa string, a missing
# binary, an ELF it cannot load -- produces an empty signature that looks
# exactly like a hang. Report its own words first; guessing at a trap when
# the real answer was "bad --isa option: unsupported extension" costs a lot
# more time than this check.
if grep -qi "^error\|bad --isa\|unsupported extension" /tmp/spike_raw.txt; then
	echo "ERROR: spike did not start:" >&2
	grep -i -m3 "^error\|bad --isa\|unsupported extension" /tmp/spike_raw.txt >&2
	exit 1
fi

# An exception here means the *test* is malformed (an illegal encoding),
# not that the DUT is wrong -- worth saying out loud, since the resulting
# signature would otherwise just look like a mismatch.
if grep -q "exception" /tmp/spike_raw.txt; then
	echo "WARNING: spike hit an exception -- suspect the test, not the DUT:" >&2
	grep -B2 "exception" /tmp/spike_raw.txt | head -6 >&2
fi

# Debug-mode `mem` prints one bare hex doubleword per line (prefixed by the
# "(spike)" prompt on the same line). Split each into two 32-bit words, low
# first, to match DoomV's signature.log granularity of 4.
python3 - > "$TEST.spike.sig" <<'PY'
import re
vals = []
for line in open("/tmp/spike_raw.txt"):
    line = line.replace("(spike)", "").strip()
    if re.fullmatch(r"0x[0-9a-f]+", line):
        vals.append(int(line, 16))
for v in vals:
    print("%08x" % (v & 0xFFFFFFFF))
    print("%08x" % ((v >> 32) & 0xFFFFFFFF))
PY

echo "$TEST.spike.sig words: $(wc -l < "$TEST.spike.sig")" >&2
