#!/usr/bin/env bash
# Cross-compile llama.cpp for the guest, and put the binary in shared/.
#
# Run from the repository root, on the Windows side:  bash tools/llama/build.sh
# It does its work inside WSL, where the cross-compiler lives, and writes one
# file: shared/llama.
#
# Static, because the guest has its own glibc and this one is not it. No
# OpenMP, because libgomp has no static cross build here and the link fails on
# it -- llama.cpp falls back to its own pthread pool, which is what you want on
# a single-hart machine anyway. No curl, because nothing here downloads models.
# GGML_NATIVE=OFF because the build machine is x86 and the target is not.
set -euo pipefail
cd "$(dirname "$0")/../.."
# Git Bash gives /z/Code/... for Z:\Code\...; WSL sees the same drive under
# /mnt, so the whole conversion is one prefix.
WSLREPO="/mnt$(pwd)"
DISTRO="${DISTRO:-Ubuntu}"
REF="${LLAMA_REF:-master}"

wsl.exe -d "$DISTRO" -u root -- bash -c "
set -euo pipefail

# The C++ cross-compiler is a separate package from the C one, and only the C
# one comes with the RISC-V toolchain this project already installs.
for pkg in gcc-riscv64-linux-gnu g++-riscv64-linux-gnu cmake git; do
    dpkg -s \$pkg >/dev/null 2>&1 || apt-get install -y \$pkg
done

cd /root
[ -d llama.cpp ] || git clone --depth 1 https://github.com/ggml-org/llama.cpp.git
cd llama.cpp
git fetch --depth 1 origin '$REF' && git checkout FETCH_HEAD 2>/dev/null || true

cmake -B build-riscv \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
  -DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=riscv64-linux-gnu-g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF \
  -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_TESTS=OFF \
  -DCMAKE_C_FLAGS='-march=rv64gc' -DCMAKE_CXX_FLAGS='-march=rv64gc' \
  -DCMAKE_EXE_LINKER_FLAGS='-static'

# llama-app is the single 'llama' binary with subcommands; there has been no
# llama-cli target since the CLI was folded into it.
cmake --build build-riscv --target llama-app -j\$(nproc)

riscv64-linux-gnu-strip -o '$WSLREPO/shared/llama' build-riscv/bin/llama
"

ls -la shared/llama
file shared/llama 2>/dev/null || true
echo
echo "Now put a .gguf in shared/ and see tools/llama/README.md for running it."
