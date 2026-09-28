#!/bin/bash
# WSL half of gen_reference.sh -- see there for why each pin is what it is.
set -u
HERE="$1"; EXT="${2:-}"
AT=/mnt/z/Code/Dev/DoomV/tools/verification/tests/arch-test
VENV=/root/act-venv
export PATH=/usr/local/bin:/usr/bin:/bin

[ -x "$VENV/bin/act" ] || { echo "act not installed -- run setup.sh" >&2; exit 2; }

cd "$AT" || exit 2
exec "$VENV/bin/act" config/sail/sail-RVA23S64/test_config.yaml \
	--workdir "$HERE/work" \
	--test-dir tests \
	${EXT:+--extensions "$EXT"} \
	-k
