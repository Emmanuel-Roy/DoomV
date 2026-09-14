#!/usr/bin/env bash
# Compatibility entry point; maintained automation lives in scripts/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "${PYTHON:-python}" "$ROOT/scripts/verify.py" "$@"
