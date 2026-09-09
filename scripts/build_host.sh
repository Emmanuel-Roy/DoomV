#!/usr/bin/env bash
# Native Windows emulator. Called by the Python entry points.
set -euo pipefail
cd "$(dirname "$0")/.."
for tool in make gcc g++; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; run scripts/install_dependencies.ps1" >&2; exit 2; }
done
# Rebuild without deleting the executable or stopping its owner.
make -B -j"${JOBS:-4}"
