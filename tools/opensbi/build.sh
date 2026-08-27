#!/usr/bin/env bash
# Builds fw_jump.elf/fw_jump.bin from the tools/opensbi/src submodule
# (pinned to v1.3 -- see README.md for why not the latest release) and
# copies the result into this directory. Run from an MSYS2 shell with
# the xpack riscv-none-elf-gcc toolchain's bin/ on PATH, or edit TC_BIN
# below.
set -e

TC_BIN="${TC_BIN:-/c/Users/royem/SWE/toolchains/xpack-riscv-none-elf-gcc-15.2.0-1/bin}"
export PATH="$TC_BIN:$PATH"

cd "$(dirname "$0")/src"

# OpenSBI v1.3's sbi_types.h does `typedef int bool` / `#define true 1` /
# `#define false 0` -- harmless under the C standard it was written
# against, but a hard error under GCC 15's C23 default, where bool/true/
# false are real keywords. Forcing back to gnu11 is simpler and more
# robust than patching every resulting type mismatch individually.
# Reverted at the end (via the trap below) so the submodule always comes
# back to a clean, unmodified, pinned-to-v1.3 working tree, whether the
# build succeeds or fails.
trap 'git checkout -- Makefile' EXIT
sed -i 's/^CFLAGS\t\t=\t-g /CFLAGS\t\t=\t-g -std=gnu11 /' Makefile

make PLATFORM=generic CROSS_COMPILE=riscv-none-elf- PLATFORM_RISCV_XLEN=64 \
     PLATFORM_RISCV_ISA=rv64imafdc_zicsr_zifencei PLATFORM_RISCV_ABI=lp64d -j4

cp build/platform/generic/firmware/fw_jump.elf ../fw_jump.elf
cp build/platform/generic/firmware/fw_jump.bin ../fw_jump.bin
echo "Built tools/opensbi/fw_jump.{elf,bin}"
