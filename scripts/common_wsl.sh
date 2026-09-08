#!/usr/bin/env bash
# Sourced by the WSL builders. ROOT is the Windows checkout's /mnt/... path.
set -euo pipefail
ROOT="$(cd "${ROOT:?repository path}" && pwd)"
TOOLS="${DOOMV_TOOLS:-$HOME/.local/share/doomv}"
# Each checkout has its own Linux-side build cache, including paths with spaces.
KEY="$(printf '%s' "$ROOT" | sha256sum | cut -c1-16)"
CACHE="${DOOMV_CACHE:-$HOME/.cache/doomv/$KEY}"
JOBS="${JOBS:-4}"
export PATH="$TOOLS/xpack/bin:$TOOLS/sail/bin:$PATH"
mkdir -p "$CACHE"

pinned_source() {
    local name="$1" module="$2" url="$3" sha dest
    sha="$(git -C "$ROOT" ls-tree HEAD "$module" | awk '{print $3}')"
    [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || { echo "No gitlink for $module" >&2; return 2; }
    dest="$CACHE/$name-$sha"
    if [ ! -d "$dest/.git" ]; then
        mkdir -p "$dest"
        git -C "$dest" init -q
        git -C "$dest" remote add origin "$url"
    fi
    if ! git -C "$dest" cat-file -e "$sha^{commit}" 2>/dev/null; then
        git -C "$dest" fetch --depth 1 origin "$sha" >&2
    fi
    if [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null || true)" != "$sha" ]; then
        git -C "$dest" -c core.autocrlf=false checkout --detach "$sha" >&2
    fi
    printf '%s\n' "$dest"
}
