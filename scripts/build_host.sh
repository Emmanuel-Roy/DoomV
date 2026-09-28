#!/usr/bin/env bash
# Native Windows emulator. Called by the Python entry points.
set -euo pipefail
cd "$(dirname "$0")/.."
# The Makefile builds with Clang when it can find one and GCC otherwise, so a
# compiler is required but neither one in particular. GCC is still what
# install_dependencies.ps1 provides, and what a checkout without
# scripts/get_clang.py having been run will use.
command -v make >/dev/null || { echo "Missing make; run scripts/install_dependencies.ps1" >&2; exit 2; }
if ! ls build/toolchains/llvm-mingw-*/bin/clang++.exe >/dev/null 2>&1    && ! command -v clang++ >/dev/null; then
    for tool in gcc g++; do
        command -v "$tool" >/dev/null || { echo "Missing $tool (and no clang); run scripts/install_dependencies.ps1" >&2; exit 2; }
    done
fi
# Rebuild without deleting the executable or stopping its owner.
make -B -j"${JOBS:-4}"
