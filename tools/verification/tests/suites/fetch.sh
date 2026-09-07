#!/usr/bin/env bash
# Fetch the precompiled test suites the Sail model's own test tree uses.
#
# These are pre-existing coverage, and they are the first thing to reach for:
# where an established suite tests something, a hand-written test of the same
# ground is weaker evidence and duplicated effort. The suites here cover
# large parts of RVA23S64 that riscv-arch-test's ACT4 does not test at all --
# most importantly the hypervisor extension, for which ACT4 has no tests, no
# testplan and no coverpoints, despite RVA23S64 requiring H.
#
#   riscv-tests                 base ISA plus rv64mi/rv64si privileged, and
#                               a few hypervisor two-stage translation tests
#   damo-rv-priv-ats            43 RV64 hypervisor groups: Sv39x4/Sv48x4/
#                               Sv57x4 two-stage translation against every
#                               guest mode, the Sh* sub-extensions, and
#                               hypervisor-side pointer masking and stateen
#   riscv-vector-tests v128x64  vector, at VLEN=128 -- which is DoomV's fixed
#                               width (Registers::VLEN_BITS), so this avoids
#                               the VLEN mismatch that made three suites
#                               disagree with Sail earlier for no good reason
#
# Precompiled ELFs, so no cross toolchain is involved. They are binaries
# rather than sources and are gitignored; re-run this to restore them.
#
# The URL and version track sail-riscv's test/CMakeLists.txt. If a fetch
# 404s, check TEST_DOWNLOAD_VERSION there -- the tarball names are exact
# (the vector one is v128x64, not v128-e64).
set -u

BASE=https://github.com/riscv-software-src/sail-riscv-tests/releases/download
VER="${TEST_DOWNLOAD_VERSION:-2026-08-20}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2

rc=0
for t in riscv-tests damo-tests riscv-vector-tests-v128x64; do
	if [ -d "$t" ] && [ -n "$(ls -A "$t" 2>/dev/null)" ]; then
		printf '  %-30s present (%s files)\n' "$t" "$(find "$t" -type f | wc -l)"
		continue
	fi
	printf '  %-30s fetching...\n' "$t"
	if curl -fsSL "$BASE/$VER/$t.tar.gz" -o "$t.tar.gz"; then
		mkdir -p "$t" && tar xzf "$t.tar.gz" -C "$t" && rm -f "$t.tar.gz"
		printf '  %-30s ok (%s files)\n' "$t" "$(find "$t" -type f | wc -l)"
	else
		printf '  %-30s FAILED\n' "$t"
		rm -f "$t.tar.gz"
		rc=1
	fi
done
exit $rc
