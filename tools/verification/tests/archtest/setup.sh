#!/usr/bin/env bash
# One-time environment bring-up for running riscv-arch-test against DoomV.
#
# Run from Git Bash on the Windows side; everything it installs lands in WSL,
# where the toolchain and the reference model live.
#
#   ./setup.sh
#
# What this installs, and why each piece is the version it is:
#
#   act (the framework)
#       Installed into a venv at /root/act-venv from the submodule's own
#       framework/ directory, so it always matches the checked-out revision.
#       The submodule is pinned to the act4 branch: the 4.0.0 tag requires
#       Sail 0.10, act4 requires 0.13.1, and 0.13.1 is the one that exists.
#
#   Sail 0.13.1
#       ACT4 pins the reference model exactly and refuses anything else.
#       Built in a worktree at /root/build/sail-0131, deliberately separate
#       from the 0.14 build the differential harness uses. The pin is not
#       bureaucracy: it is what keeps a mismatch attributable to DoomV
#       rather than to a framework/model pairing artifact.
#
#   riscv64-unknown-elf-gcc (newlib), 14.2.0
#       This must be the newlib toolchain. Pointing the framework at
#       riscv64-linux-gnu-gcc instead was tried, and it is not a near miss:
#       the tests compile, and then *Sail's own reference run* dies in a
#       trap loop before reaching the first test case. Verified both ways on
#       Zicond -- newlib SUCCESS, linux-gnu trap loop.
#
#       ACT4 asks for GCC 15 or later and Ubuntu's newlib package tops out
#       at 14.2.0, with no newer package available. The floor is lowered to
#       14 below. That is a real deviation from the framework's declared
#       environment, so it is done in the open here rather than by
#       substituting a compiler that appears to satisfy the check.
#
#   Ruby + bundler
#       UDB, which compiles the RVA23S64 profile YAML into the DUT headers
#       each test includes. Needed even though the tests themselves are
#       checked in.
#
#   Symlink materialisation
#       The arch-test checkout sits on a Windows filesystem with
#       core.symlinks false, so every file git records as a symlink arrived
#       as a text file containing its target's path. The assembler then
#       reads "../sail-rv64-max/rvmodel_macros.h" as source and reports
#       `unknown pseudo-op: '..'`. Copying the target's contents over the
#       pointer works from both sides of the WSL boundary, which a real
#       symlink would not.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
DISTRO="${DISTRO:-Ubuntu}"

win_to_wsl() {
	local p; p="$(cd "$1" && pwd -W 2>/dev/null || echo "$1")"
	local drive="${p:0:1}"
	printf '/mnt/%s%s' "$(printf '%s' "$drive" | tr 'A-Z' 'a-z')" "$(printf '%s' "${p:2}")"
}
HERE_WSL="$(win_to_wsl "$HERE")"

MSYS2_ARG_CONV_EXCL='*' wsl -d "$DISTRO" -u root -- bash "$HERE_WSL/_setup_wsl.sh" "$HERE_WSL"
