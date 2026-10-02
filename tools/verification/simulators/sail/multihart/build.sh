#!/usr/bin/env bash
# Builds sail_riscv_mh: the Sail model run as a machine of several harts.
# See README.md beside this file.
#
# The Sail submodule is not touched. Its committed sources are exported to
# build/src, multihart.patch is applied there, and the driver is added; the
# model is then generated and compiled as build.sh one directory up does for
# sail_riscv_sim. Run in WSL, as that script is.
set -euo pipefail

SAIL_DIR=/root/build/sail-bin/sail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../src" && pwd)
WORK="$HERE/build"

export PATH="$SAIL_DIR/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
command -v sail >/dev/null || { echo "sail not found in $SAIL_DIR/bin" >&2; exit 1; }

# The patch is applied to an export of exactly the submodule's commit, made
# again only when the commit or the patch changes, since a fresh tree
# regenerates and recompiles the whole model. core.autocrlf off: the patch is
# LF, as the repository is.
stamp="$(git -c safe.directory='*' -C "$SRC" rev-parse HEAD) $(sha1sum < "$HERE/multihart.patch")"
if [ "$(cat "$WORK/src.stamp" 2>/dev/null)" != "$stamp" ]; then
  rm -rf "$WORK/src" "$WORK/cmake"
  mkdir -p "$WORK/src"
  git -c safe.directory='*' -c core.autocrlf=false -C "$SRC" archive HEAD | tar -x -C "$WORK/src"
  patch -s -d "$WORK/src" -p1 < "$HERE/multihart.patch"
  echo "$stamp" > "$WORK/src.stamp"
fi
cmp -s "$HERE/sail_riscv_mh.cpp" "$WORK/src/c_emulator/sail_riscv_mh.cpp" ||
  cp "$HERE/sail_riscv_mh.cpp" "$WORK/src/c_emulator/"

cmake -S "$WORK/src" -B "$WORK/cmake" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DDOWNLOAD_GMP="${DOWNLOAD_GMP:-TRUE}" -DENABLE_RISCV_TESTS=FALSE
cmake --build "$WORK/cmake" -j"$(nproc)" --target sail_riscv_mh
echo "simulator: $WORK/cmake/c_emulator/sail_riscv_mh"
