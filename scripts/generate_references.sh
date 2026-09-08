#!/usr/bin/env bash
# WSL: consume the per-checkout tools manifest written by the installer.
set -euo pipefail
ROOT="${1:?repository path}"
EXT="${2:-}"
source "$(dirname "$0")/common_wsl.sh"
MANIFEST="$ROOT/build/verification/tools.json"
read_tool() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$MANIFEST" "$1"; }
AT="$(read_tool at)"
VENV="$(read_tool venv)"
SAIL="$(read_tool act_sail)"
CONFIG_DIR="$CACHE/act-config/sail/sail-RVA23S64"
mkdir -p "$CACHE/act-config"
# Preserve relative paths in the upstream config by copying its small config dir.
cp -a "$AT/config/." "$CACHE/act-config/"
"$VENV/bin/python" - "$CONFIG_DIR/test_config.yaml" "$TOOLS/xpack/bin/riscv-none-elf-gcc" "$TOOLS/xpack/bin/riscv-none-elf-objdump" "$SAIL" <<'PY'
from pathlib import Path
from ruamel.yaml import YAML
import sys
yaml = YAML()
path = Path(sys.argv[1])
data = yaml.load(path)
for key, value in zip(('compiler_exe', 'objdump_exe', 'ref_model_exe'), sys.argv[2:]):
    data[key] = value
yaml.dump(data, path)
PY
args=("$CONFIG_DIR/test_config.yaml" --workdir "$ROOT/tools/verification/tests/archtest/work" --test-dir "$AT/tests" -k)
if [ -n "$EXT" ]; then args+=(--extensions "$EXT"); fi
cd "$AT"
exec "$VENV/bin/act" "${args[@]}"
