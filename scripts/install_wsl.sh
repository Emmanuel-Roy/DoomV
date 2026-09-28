#!/usr/bin/env bash
# Ubuntu half of install_dependencies.ps1. Does not run any emulator tests.
set -euo pipefail
ROOT="${1:?repository path}"
source "$(dirname "$0")/common_wsl.sh"
export DEBIAN_FRONTEND=noninteractive
[ "$(uname -m)" = x86_64 ] || { echo 'This installer currently supports x86-64 WSL.' >&2; exit 2; }
apt-get update
apt-get install -y build-essential git curl ca-certificates xz-utils unzip \
    flex bison bc libssl-dev libelf-dev device-tree-compiler cpio rsync \
    gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu python3 python3-venv \
    cmake ninja-build libgmp-dev zlib1g-dev pkg-config ruby-full ruby-bundler
mkdir -p "$TOOLS"

install_archive() {
    local url="$1" sha="$2" dest="$3" archive
    if [ -f "$dest/.installed-$sha" ]; then return; fi
    archive="$(mktemp "$TOOLS/download.XXXXXX")"
    curl --fail --location --retry 3 "$url" -o "$archive"
    printf '%s  %s\n' "$sha" "$archive" | sha256sum --check -
    mkdir -p "$dest"
    tar -xzf "$archive" -C "$dest" --strip-components=1
    rm -f -- "$archive"
    touch "$dest/.installed-$sha"
}

echo '=== xPack GCC 15.2.0 (newlib, including nosys.specs for DOOM) ==='
install_archive \
    https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v15.2.0-1/xpack-riscv-none-elf-gcc-15.2.0-1-linux-x64.tar.gz \
    aaaa8060c914851a3e5ee1ba82cc3d6f80972f90638a05c6e823a37557a33758 "$TOOLS/xpack"
riscv-none-elf-gcc --version

if [ "${2:-}" != --skip-verification ]; then
    echo '=== Sail compiler 0.20.2, including z3 ==='
    install_archive \
        https://github.com/rems-project/sail/releases/download/0.20.2-binary/sail-Linux-x86_64.tar.gz \
        26b59bcab2d66e9f220d317dfe45f8b09170ed70e59a824553d6f525134d1ff6 "$TOOLS/sail"
    sail --version
    bash "$ROOT/scripts/setup_verification.sh" "$ROOT"
    bash "$ROOT/tools/verification/tests/suites/fetch.sh"
fi
