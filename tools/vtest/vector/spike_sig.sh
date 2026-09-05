#!/usr/bin/env bash
# Builds vtest_v.S and dumps spike's signature region, in the same format
# DoomV's -sig writes: one 32-bit word per line, low word of each doubleword
# first.
#
# Uses spike's debug mode rather than its +signature plusarg. The plusarg is
# the documented arch-test path, but this spike build never populates
# sig_file from it (htif_t::stop() then sees an empty filename and writes
# nothing, silently, while still exiting 0). Driving `until pc` + `mem` from
# a command file is version-proof and needs no special build.
#
# Run inside WSL. Args: [elf_dir]  (default: this script's directory)
set -euo pipefail

DIR="${1:-$(cd "$(dirname "$0")" && pwd)}"
cd "$DIR"

CC=riscv64-linux-gnu-gcc
NM=riscv64-linux-gnu-nm
SPIKE="${SPIKE:-/root/build/spike-build/spike}"
ISA="${ISA:-rv64gcv_zvl128b_zicsr_zifencei}"

$CC -march=rv64gcv -mabi=lp64d -static -mcmodel=medany -nostdlib -nostartfiles \
    -T vtest_v.lds -o vtest_v.elf vtest_v.S 2>&1 | grep -v RWX || true

sym() { $NM vtest_v.elf | awk -v s="$1" '$3==s {print $1}'; }
HALT=$(sym sig_done)
BEG=$(sym begin_signature)
END=$(sym end_signature)
echo "halt=0x$HALT begin=0x$BEG end=0x$END" >&2

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
timeout 120 "$SPIKE" -d --debug-cmd=/tmp/spike_cmds.txt --isa="$ISA" ./vtest_v.elf > /tmp/spike_raw.txt 2>&1 || true

# Debug-mode `mem` prints one bare hex doubleword per line (prefixed by the
# "(spike)" prompt on the same line). Split each into two 32-bit words, low
# first, to match DoomV's signature.log granularity of 4.
python3 - > spike.sig <<'PY'
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

echo "spike.sig words: $(wc -l < spike.sig)" >&2
