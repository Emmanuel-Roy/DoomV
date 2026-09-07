#!/usr/bin/env bash
# Stage 1: compile the riscv-arch-test RVA23S64 suite and produce Sail's
# reference signature for each test.
#
# Runs inside WSL, where the toolchain and Sail live, but writes into the
# repo's own work/ directory rather than somewhere under /root -- DoomV is a
# native .exe and has to read the same ELFs, so they must land on a
# filesystem both sides can see.
#
#   ./gen_reference.sh              # everything the config covers
#   ./gen_reference.sh Zicond,Zba   # a comma-separated subset
#
# Environment notes, all of which are deviations worth knowing about:
#
#   * Sail is pinned. ACT4 requires exactly 0.13.1 and refuses anything
#     else. That build lives at /root/build/sail-0131 and is separate from
#     the 0.14 the differential harness uses -- the pin exists so that a
#     mismatch is a DoomV bug rather than a framework/model pairing
#     artifact, which is a distinction this project has already been bitten
#     by more than once.
#   * The compiler is Ubuntu's newlib riscv64-unknown-elf-gcc 14.2.0, while
#     ACT4 asks for 15 or later. Substituting riscv64-linux-gnu-gcc 15.2.0
#     was tried and is wrong: it builds, and then Sail's own reference run
#     dies in a trap loop before reaching the first test. setup.sh lowers
#     the floor to 14 deliberately and says so.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DISTRO="${DISTRO:-Ubuntu}"

win_to_wsl() {
	local p; p="$(cd "$1" && pwd -W 2>/dev/null || echo "$1")"
	local drive="${p:0:1}"
	printf '/mnt/%s%s' "$(printf '%s' "$drive" | tr 'A-Z' 'a-z')" "$(printf '%s' "${p:2}")"
}
HERE_WSL="$(win_to_wsl "$HERE")"
EXT="${1:-}"

MSYS2_ARG_CONV_EXCL='*' wsl -d "$DISTRO" -u root -- \
	bash "$HERE_WSL/_gen_reference_wsl.sh" "$HERE_WSL" "$EXT"
