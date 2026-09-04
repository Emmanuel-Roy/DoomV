#!/usr/bin/env bash
# Builds fw_jump.elf/fw_jump.bin from the tools/opensbi/src submodule
# (pinned to v1.3 -- see README.md for why not the latest release) and
# copies the result into this directory. Run from a POSIX shell (MSYS2 or
# Git Bash) with riscv-none-elf-gcc reachable: either already on PATH, or
# via TC_BIN=/path/to/toolchain/bin. See PLAN.md for the xpm install that
# puts it in the xPacks store.
set -e

if [ -n "${TC_BIN:-}" ]; then
	export PATH="$TC_BIN:$PATH"
fi

if ! command -v riscv-none-elf-gcc >/dev/null 2>&1; then
	echo "error: riscv-none-elf-gcc not found on PATH." >&2
	echo "       Add the toolchain bin/ to PATH, or set TC_BIN=/path/to/bin." >&2
	exit 1
fi

# OpenSBI's kconfig scripts run via `#!/usr/bin/env python3`.
if ! command -v python3 >/dev/null 2>&1; then
	echo "error: python3 not found on PATH (OpenSBI's kconfig needs it)." >&2
	echo "       Windows Python often installs only python.exe -- put a" >&2
	echo "       python3 shim forwarding to it on PATH." >&2
	exit 1
fi

cd "$(dirname "$0")/src"

# Both edits below are reverted by the trap, so the submodule always comes
# back to a clean, unmodified, pinned-to-v1.3 working tree whether the
# build succeeds or fails.
trap 'git checkout -- Makefile' EXIT

# OpenSBI v1.3's sbi_types.h does `typedef int bool` / `#define true 1` /
# `#define false 0` -- harmless under the C standard it was written
# against, but a hard error under GCC 15's C23 default, where bool/true/
# false are real keywords. Forcing back to gnu11 is simpler and more
# robust than patching every resulting type mismatch individually.
sed -i 's/^CFLAGS\t\t=\t-g /CFLAGS\t\t=\t-g -std=gnu11 /' Makefile

# Windows: libplatsbi.a is archived by passing ~132 absolute object paths
# in one recipe (~10.7KB). GNU Make truncates the recipe when it spawns the
# shell -- the 8191-character cmd.exe command-line limit -- and the cut is
# SILENT and lands on a clean path boundary, so ar builds a perfectly valid
# archive from the ~100 paths that survived and drops the tail, platform.o
# among them. The failure then surfaces much later, looking unrelated:
#     fw_base.S:252: undefined reference to `platform'
# (Confirmed it is make, not the shell: running the identical 10715-char
# command directly in bash archives all 132 objects fine.)
#
# So the object list must not travel through the recipe at all. $(file >)
# makes Make itself write the response file at expansion time, leaving a
# short recipe, and GNU ar expands the @file on its own. A no-op on
# Linux/macOS, where the long recipe would have been fine anyway.
sed -i 's|^\(\t *\)$(AR) $(ARFLAGS) $(1) $(2)$|\1$(file >$(1).rsp,$(2))$(AR) $(ARFLAGS) $(1) @$(1).rsp|' Makefile

make PLATFORM=generic CROSS_COMPILE=riscv-none-elf- PLATFORM_RISCV_XLEN=64 \
     PLATFORM_RISCV_ISA=rv64imafdc_zicsr_zifencei PLATFORM_RISCV_ABI=lp64d -j4

cp build/platform/generic/firmware/fw_jump.elf ../fw_jump.elf
cp build/platform/generic/firmware/fw_jump.bin ../fw_jump.bin
echo "Built tools/opensbi/fw_jump.{elf,bin}"
