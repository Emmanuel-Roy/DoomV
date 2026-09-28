#!/usr/bin/env bash
# WSL: separate pinned Sail models and an ACT venv for this checkout.
set -euo pipefail
ROOT="${1:?repository path}"
source "$(dirname "$0")/common_wsl.sh"
AT="$(pinned_source arch-test tools/verification/tests/arch-test https://github.com/riscv-non-isa/riscv-arch-test.git)"
SAIL_SRC="$(pinned_source sail tools/verification/simulators/sail/src https://github.com/riscv/sail-riscv.git)"
SAIL_ACT="$CACHE/sail-0.13.1"
if [ ! -d "$SAIL_ACT/.git" ]; then
    git clone --depth 1 --branch 0.13.1 https://github.com/riscv/sail-riscv.git "$SAIL_ACT"
fi
for model in "$SAIL_SRC" "$SAIL_ACT"; do
    cmake -S "$model" -B "$model/build" -DCMAKE_BUILD_TYPE=Release -DENABLE_RISCV_TESTS=FALSE
    cmake --build "$model/build" -j"$JOBS"
done
test "$("$SAIL_ACT/build/c_emulator/sail_riscv_sim" --version)" = 0.13.1
python3 -m venv "$CACHE/act-venv"
"$CACHE/act-venv/bin/pip" install --upgrade pip
"$CACHE/act-venv/bin/pip" install "$AT/framework"

# Leave the upstream compiler floor intact. Use GCC 15 newlib explicitly.
mkdir -p "$ROOT/build/verification"
export DOOMV_AT="$AT" DOOMV_SAIL="$SAIL_SRC/build/c_emulator/sail_riscv_sim"
export DOOMV_ACT_SAIL="$SAIL_ACT/build/c_emulator/sail_riscv_sim" DOOMV_VENV="$CACHE/act-venv"
python3 - "$ROOT/build/verification/tools.json" <<'PY'
import json, os, sys
from pathlib import Path
data = {key: os.environ['DOOMV_' + key.upper()] for key in ('at', 'sail', 'act_sail', 'venv')}
Path(sys.argv[1]).write_text(json.dumps(data, indent=2) + '\n')
PY
SAIL="$DOOMV_SAIL" SAIL_PROFILE="$AT/config/sail/sail-RVA23S64/sail-RVA23S64.yaml" \
    python3 "$ROOT/tools/verification/simulators/sail/mkconfig.py"
echo 'Reference tools ready. Next: python scripts/prepare_verification.py'
