#!/usr/bin/env bash
# Builds spike (the RISC-V ISA simulator used as the reference model in
# tools/vtest/) from the tools/spike/src submodule, pinned to a specific
# commit (see README.md -- no tagged release was used, just whatever was
# current when this was set up). Must run under MSYS2's *own* gcc/g++
# (/usr/bin, not ucrt64/clangarm64/mingw) -- see README.md for why.
set -e

cd "$(dirname "$0")/src"

# MSYS's own machine/types.h defines a conflicting `addr_t` (char*) behind
# this same guard macro that spike's fesvr/memif.h uses for its own
# (uint64_t-based) addr_t -- whichever header wins the race decides every
# subsequent translation unit's addr_t, causing hard-to-diagnose type
# errors in a handful of .cc files depending on unrelated include-order
# differences. Reverted at the end so the submodule stays pinned clean.
trap 'git checkout -- fesvr/memif.h' EXIT
sed -i '/^#define __MEMIF_H$/a #define __addr_t_defined' fesvr/memif.h

# git can't check out a real symlink without Windows dev-mode/admin
# privileges enabled -- spike_dasm_option_parser.cc (the only symlink in
# this tree) ends up as a literal text file containing its own link
# target path instead of real source. This fix is left in place
# (not reverted) since re-running `git checkout` on this one file would
# just recreate the same broken state on this filesystem -- the
# submodule will always show this one file as locally modified.
target=spike_dasm/spike_dasm_option_parser.cc
if ! grep -q '#include' "$target" 2>/dev/null; then
	cp fesvr/option_parser.cc "$target"
fi

mkdir -p ../build
cd ../build
CC=/usr/bin/gcc CXX=/usr/bin/g++ ../src/configure
make CPPFLAGS="-D__addr_t_defined -D_GNU_SOURCE" -j4

strip -o ../spike.exe spike.exe
echo "Built tools/spike/spike.exe"
