#!/usr/bin/env bash
# The WSL half of build_vulkan.sh: unpack the sysroot, install glslc, build.
#   build_vulkan_wsl.sh <repo, as WSL sees it> <debs dir under it> [llama.cpp ref]
set -euo pipefail
REPO="$1"; DEBS="$REPO/$2"; REF="${3:-}"
S=/root/sysroot-noble-riscv64

if [ ! -f "$S/usr/include/spirv/unified1/spirv.hpp" ]; then
    mkdir -p "$S"
    for f in "$DEBS"/riscv64/*.deb "$DEBS"/headers/*.deb; do dpkg-deb -x "$f" "$S"; done
fi
command -v glslc >/dev/null || dpkg -i "$DEBS"/amd64/*.deb
for pkg in gcc-riscv64-linux-gnu g++-riscv64-linux-gnu cmake git; do
    dpkg -s "$pkg" >/dev/null 2>&1 || apt-get install -y "$pkg"
done

cd /root
[ -d llama.cpp ] || git clone --depth 1 https://github.com/ggml-org/llama.cpp.git
cd llama.cpp
if [ -n "$REF" ]; then git fetch --depth 1 origin "$REF" && git checkout FETCH_HEAD; fi
git log -1 --format='llama.cpp %h %cs'

# CMAKE_SYSROOT points the compiler, the linker and find_package(Vulkan) at
# the 24.04 sysroot. glslc and the shader generator run on the build machine,
# so programs are looked for outside it (llama.cpp builds the generator with
# a host toolchain of its own when cross-compiling).
cmake -B build-riscv-vulkan \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=riscv64 \
  -DCMAKE_SYSROOT="$S" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_C_COMPILER=riscv64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=riscv64-linux-gnu-g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_VULKAN=ON -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF \
  -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_TESTS=OFF \
  -DCMAKE_C_FLAGS='-march=rv64gc' -DCMAKE_CXX_FLAGS='-march=rv64gc' \
  -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc'

cmake --build build-riscv-vulkan --target llama-app -j"$(nproc)"

OUT="$REPO/shared/llama-vulkan"
riscv64-linux-gnu-strip -o "$OUT" build-riscv-vulkan/bin/llama
riscv64-linux-gnu-readelf -d "$OUT" | grep NEEDED
echo -n 'needs glibc up to: '
riscv64-linux-gnu-objdump -T "$OUT" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
