#!/usr/bin/env bash
# Guest compiler runs in WSL; the resulting ELF runs in the Windows emulator.
set -euo pipefail
ROOT="${1:?repository path}"
GAME="${2:-free}"
WAD="${3:?WAD path}"
export PATH="${DOOMV_TOOLS:-$HOME/.local/share/doomv}/xpack/bin:$PATH"
command -v riscv-none-elf-gcc >/dev/null || { echo "Run scripts/install_dependencies.ps1 first" >&2; exit 2; }
test -f "$ROOT/tools/doom/doombuild/doomgeneric/doomgeneric/d_main.c" || {
    echo "doomgeneric submodule missing; run scripts/install_dependencies.ps1" >&2; exit 2;
}
# Use the actual WAD size, not a constant for a possibly different game release.
make -C "$ROOT/tools/doom/doombuild" -B "doomv-$GAME.elf" WAD="$GAME" WAD_LENGTH="$(wc -c < "$WAD")"
