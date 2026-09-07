#!/usr/bin/env bash
# Builds the Sail RISC-V model's C simulator, for use as a second reference
# alongside spike.
#
# Sail is the formal RISC-V specification: the model here is generated from
# the same source the architecture is defined in, rather than being an
# independent reimplementation. That makes it a stronger oracle than spike
# for profile-level questions -- and riscv-arch-test only ships RVA23S64
# configurations for Sail, not for spike, which is why it is worth having.
#
# The Sail compiler itself comes from a binary release rather than opam:
# building it from source pulls in a full OCaml toolchain, and the release
# tarball bundles the z3 the compiler needs.
set -euo pipefail

SAIL_DIR=/root/build/sail-bin/sail
SRC=$(cd "$(dirname "$0")/src" && pwd)

# z3 has to be on PATH, not merely present: sail shells out to it and
# reports a bare "SMT solver returned unexpected status 127" if it is not.
export PATH="$SAIL_DIR/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

command -v sail >/dev/null || { echo "sail not found in $SAIL_DIR/bin" >&2; exit 1; }
sail --version

cd "$SRC"

# The submodule's own build_simulator.sh is not used. core.autocrlf gives it
# CRLF line endings on checkout, which makes its "#!/bin/sh" shebang
# unrunnable ("No such file or directory", pointing at the interpreter
# rather than the script). Fixing that would mean editing a submodule's
# working tree, so the two cmake calls it wraps are issued directly instead
# -- there is nothing else in it.
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo       -DDOWNLOAD_GMP="${DOWNLOAD_GMP:-TRUE}" -DENABLE_RISCV_TESTS=FALSE
cmake --build build -j"$(nproc)"
echo "simulator: $SRC/build/c_emulator/sail_riscv_sim"
