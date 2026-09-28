#!/bin/bash
# WSL half of setup.sh. See there for why each version is what it is.
set -u
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export DEBIAN_FRONTEND=noninteractive

HERE="${1:?usage: _setup_wsl.sh <archtest dir, wsl path>}"
AT="$(cd "$HERE/../arch-test" && pwd)"
SAIL_SRC=/mnt/z/Code/Dev/DoomV/tools/verification/simulators/sail/src
SAIL_WT=/root/build/sail-0131
VENV=/root/act-venv

step() { printf '\n=== %s ===\n' "$1"; }

git config --global --add safe.directory '*' 2>/dev/null || true

step "distro packages"
# python3-venv is separate on Debian/Ubuntu; without it `python3 -m venv`
# fails at ensurepip with a message that looks like a python bug.
apt-get install -y python3-venv gcc-riscv64-unknown-elf \
	binutils-riscv64-unknown-elf ruby-full ruby-bundler cmake >/dev/null 2>&1
for t in riscv64-unknown-elf-gcc riscv64-unknown-elf-objdump ruby bundle cmake; do
	printf '  %-32s %s\n' "$t" "$(command -v "$t" || echo MISSING)"
done

step "sail 0.13.1"
if [ ! -x "$SAIL_WT/build/c_emulator/sail_riscv_sim" ]; then
	export PATH="/root/build/sail-bin/sail/bin:$PATH"   # z3 must be on PATH, not merely present
	[ -d "$SAIL_WT" ] || ( cd "$SAIL_SRC" && git worktree add --detach "$SAIL_WT" 0.13.1 )
	( cd "$SAIL_WT" \
	  && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	           -DDOWNLOAD_GMP=TRUE -DENABLE_RISCV_TESTS=FALSE >/dev/null \
	  && cmake --build build -j"$(nproc)" >/dev/null ) || { echo "sail build failed" >&2; exit 1; }
fi
ln -sf "$SAIL_WT/build/c_emulator/sail_riscv_sim" /usr/local/bin/sail_riscv_sim
echo "  sail_riscv_sim $(sail_riscv_sim --version)"

step "materialise symlinks in the arch-test checkout"
cd "$AT" || exit 2
n=0
while read -r f; do
	[ -f "$f" ] || continue
	[ "$(wc -l < "$f")" -le 1 ] || continue          # already materialised
	target="$(head -1 "$f")"
	case "$target" in */*) ;; *) continue ;; esac
	d="$(dirname "$f")"
	[ -f "$d/$target" ] || continue
	cp "$d/$target" "$f.tmp" && mv "$f.tmp" "$f" && n=$((n+1))
done < <(git ls-files -s | awk '$1=="120000" {print $4}')
echo "  materialised $n"

step "act framework"
[ -x "$VENV/bin/act" ] || {
	python3 -m venv "$VENV" \
	&& "$VENV/bin/pip" install -q --upgrade pip \
	&& "$VENV/bin/pip" install -q "$AT/framework"
} || { echo "act install failed" >&2; exit 1; }

# ACT4 requires GCC >= 15; Ubuntu's newlib toolchain is 14.2.0 and there is
# no newer package. 14.2.0 produces a correct Sail reference run, so the
# floor is lowered here -- visibly, and only after checking that the
# alternative (a linux-gnu compiler that does satisfy the check) is wrong.
CFG="$(echo "$VENV"/lib/python3*/site-packages/act/config.py)"
if [ -f "$CFG" ]; then
	sed -i 's/^REQUIRED_GCC_MAJOR_VERSION = 15$/REQUIRED_GCC_MAJOR_VERSION = 14  # Ubuntu newlib tops out at 14.2.0; see setup.sh/' "$CFG"
	grep -n "^REQUIRED_GCC_MAJOR_VERSION" "$CFG" | sed 's/^/  /'
fi

step "ready"
echo "  next: ./gen_reference.sh Zicond   then   python archtest.py Zicond"
