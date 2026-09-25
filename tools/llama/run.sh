#!/usr/bin/env bash
# Run a GGUF model in the guest, unattended, and print what it generates.
#
#   bash tools/llama/run.sh shared/model.gguf
#   bash tools/llama/run.sh shared/model.gguf "Once upon a time" 32
#   RAM=8G bash tools/llama/run.sh shared/big.gguf
#
# Needs shared/llama (bash tools/llama/build.sh) and an ubuntu.img. It boots
# Ubuntu on a copy of the image, mounts the share, copies the binary in, runs
# the model and powers the machine off, so the run ends by itself.
#
# It generates the -input script rather than shipping one per model: the model
# name and prompt have to be typed into the guest's shell, so they are part of
# the script text.
set -euo pipefail
cd "$(dirname "$0")/.."/..

MODEL_PATH="${1:?usage: run.sh <path-to.gguf-under-shared> [prompt] [n_tokens]}"
PROMPT="${2:-The capital of France is}"
NTOK="${3:-12}"
RAM="${RAM:-4G}"
IMAGE="${IMAGE:-ubuntu.img}"
WORK=build/llama
OUT="$WORK/run-$(basename "${MODEL_PATH%.gguf}").log"

[ -f "$MODEL_PATH" ] || { echo "no such model: $MODEL_PATH" >&2; exit 1; }
[ -f shared/llama ]  || { echo "shared/llama is missing; run: bash tools/llama/build.sh" >&2; exit 1; }
[ -f "$IMAGE" ]      || { echo "$IMAGE is missing; see tools/linux/ubuntu/README.md" >&2; exit 1; }

# The guest sees the share at /mnt/shared, so only the name matters, not the
# host path it was given as.
MODEL="/mnt/shared/$(basename "$MODEL_PATH")"
mkdir -p "$WORK"

# sleep N is N x 10,000 instructions, not wall-clock, so this script behaves
# the same on any host. Generous rather than tuned: the run ends when the guest
# powers off, so an early wake costs nothing.
cat > "$WORK/input.script" <<EOF
sleep 20000
type root
key 28 1
key 28 0
sleep 30000
type doomv
key 28 1
key 28 0
sleep 75000
type mkdir -p /mnt/shared && mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared && cp /mnt/shared/llama /root/llama && chmod +x /root/llama && echo LLAMA-READY > /dev/hvc0
key 28 1
key 28 0
sleep 600000
type /root/llama completion -m $MODEL -p "$PROMPT" -n $NTOK -t 1 --no-warmup > /dev/hvc0 2>&1; echo LLAMA-RUN-DONE > /dev/hvc0; echo o > /proc/sysrq-trigger
key 28 1
key 28 0
sleep 9000000
EOF

# On a copy: a boot writes to the disk, and the real image should not be what
# a throwaway run leaves behind.
cp "$IMAGE" "$WORK/ubuntu-run.img"

echo "model  $MODEL"
echo "prompt $PROMPT   ($NTOK tokens, -ram=$RAM)"
echo "log    $OUT"
echo "This takes tens of minutes: a systemd boot, then the model load, then"
echo "generation at emulated speed. It ends when the guest powers off."
echo

./riscv_doom.exe -ng "-ram=$RAM" \
    -opensbi=build/linux/fw_jump.elf -kernel=build/linux/Image \
    -dtb=build/linux/ubuntu.dtb "-disk=$WORK/ubuntu-run.img" \
    -drives= -shared=shared \
    -expect="doomv login:" "-input=$WORK/input.script" > "$OUT" 2>&1 || true

echo "--- what the model produced ---"
# The console log carries the kernel's output and the shell's too; the answer
# is between the prompt echo and the done marker.
strings "$OUT" | sed -n '/^assistant$/,/LLAMA-RUN-DONE/p' | sed '/LLAMA-RUN-DONE/d' || true
echo "--- full log: $OUT ---"
