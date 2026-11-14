#!/usr/bin/env bash
# Run a GGUF model in the guest on this computer's GPU, unattended, and print
# what it generates.
#
#   bash tools/llama/run_vulkan.sh shared/model.gguf
#   bash tools/llama/run_vulkan.sh shared/model.gguf "Once upon a time" 32
#   NGL=10 RAM=8G bash tools/llama/run_vulkan.sh shared/big.gguf
#
# The path to llama.cpp's Vulkan backend: llama.cpp in the guest -> the Vulkan
# loader -> Mesa's Venus driver (virtio_icd.json) -> virtio-gpu -> DoomV -gpu
# -> virglrenderer's Venus renderer -> the host's Vulkan driver.
#
# Needs shared/llama-vulkan (bash tools/llama/build_vulkan.sh), an ubuntu.img
# with Mesa's Vulkan drivers (python scripts/boot.py ubuntu --setup-gpu, once)
# and build/venus (python scripts/get_venus.py, which boot.py --gpu runs by
# itself). Like run.sh it boots a copy of the image, so the real one is not
# changed, and powers off when done.
#
#   NGL=   layers to put on the GPU (default 99: all of them; 0 = CPU only,
#          the same binary, for comparing -- two runs can go side by side)
#   CTX=   context length (default 2048: llama.cpp's default is the model's
#          full training context, whose KV cache would go to the GPU too)
#   VKDEV= which of the guest's Vulkan devices llama.cpp uses
#          (GGML_VK_VISIBLE_DEVICES; default 0, the host's first GPU). Venus
#          shows the guest every GPU the host has -- a discrete card and an
#          integrated one are two -- and llama.cpp otherwise splits the model
#          across all of them. "0,1" uses both; vulkaninfo's list is the order.
#   SEED=  the sampler's seed (default 42), so a run can be repeated
#   RAM=, IMAGE=   as run.sh
#   DOOMV= the emulator (default ./riscv_doom.exe; it loads Venus from the
#          build/venus/bin beside it)
set -euo pipefail
. "$(dirname "$0")/in_git_bash.sh"
cd "$(dirname "$0")/.."/..

MODEL_PATH="${1:?usage: run_vulkan.sh <path-to.gguf-under-shared> [prompt] [n_tokens]}"
PROMPT="${2:-The capital of France is}"
NTOK="${3:-12}"
RAM="${RAM:-4G}"
NGL="${NGL:-99}"
CTX="${CTX:-2048}"
VKDEV="${VKDEV:-0}"
SEED="${SEED:-42}"
# NGL=0 is the CPU baseline: without --no-op-offload llama.cpp still sends
# big matrix products to the GPU.
OPS=""; [ "$NGL" = 0 ] && OPS="--no-op-offload"
IMAGE="${IMAGE:-ubuntu.img}"
DOOMV="${DOOMV:-./riscv_doom.exe}"
WORK=build/llama
OUT="$WORK/run-vulkan-$(basename "${MODEL_PATH%.gguf}")-ngl$NGL.log"

[ -f "$MODEL_PATH" ]          || { echo "no such model: $MODEL_PATH" >&2; exit 1; }
[ -f shared/llama-vulkan ]    || { echo "shared/llama-vulkan is missing; run: bash tools/llama/build_vulkan.sh" >&2; exit 1; }
[ -f "$IMAGE" ]               || { echo "$IMAGE is missing; see tools/linux/ubuntu/README.md" >&2; exit 1; }
[ -f "$(dirname "$DOOMV")/build/venus/bin/libvirglrenderer-1.dll" ] \
    || { echo "no Venus beside $DOOMV; run: python scripts/get_venus.py" >&2; exit 1; }

MODEL="/mnt/shared/$(basename "$MODEL_PATH")"
mkdir -p "$WORK"

# Every guest command reports to /dev/hvc0, the console this log captures.
# vulkaninfo first, so a run that fails says whether the guest had a GPU at
# all: "Virtio-GPU Venus (<host GPU>)" is the device llama.cpp should find.
# A missing ICD, or an image without --setup-gpu, shows up there rather than
# as llama.cpp quietly falling back to the CPU.
#
# sleep N is N x 10,000 instructions, not wall-clock; generous, since the run
# ends when the guest powers off.
TAG="$(basename "${MODEL_PATH%.gguf}")-ngl$NGL"
SCRIPT="$WORK/input-vulkan-$TAG.script"
RUNIMG="$WORK/ubuntu-run-vulkan-$TAG.img"
cat > "$SCRIPT" <<EOF
sleep 20000
type root
key 28 1
key 28 0
sleep 30000
type doomv
key 28 1
key 28 0
sleep 75000
type mkdir -p /mnt/shared && mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared && cp /mnt/shared/llama-vulkan /root/llama-vulkan && chmod +x /root/llama-vulkan && echo LLAMA-READY > /dev/hvc0
key 28 1
key 28 0
sleep 600000
type (ls --color=never /dev/dri; vulkaninfo --summary 2>&1 | grep --color=never -E 'deviceName|driverName|driverInfo|apiVersion|ERROR') > /dev/hvc0 2>&1; echo VULKANINFO-DONE > /dev/hvc0
key 28 1
key 28 0
sleep 300000
type export GGML_VK_VISIBLE_DEVICES=$VKDEV; /root/llama-vulkan completion --list-devices > /dev/hvc0 2>&1; /root/llama-vulkan completion -m $MODEL -p "$PROMPT" -n $NTOK -c $CTX -ngl $NGL -t 1 --no-warmup -st -lv 3 --log-colors off --seed $SEED $OPS > /dev/hvc0 2>&1; echo LLAMA-RUN-DONE \$? > /dev/hvc0; echo o > /proc/sysrq-trigger
key 28 1
key 28 0
sleep 9000000
EOF

cp "$IMAGE" "$RUNIMG"

echo "model  $MODEL"
echo "prompt $PROMPT   ($NTOK tokens, -ngl $NGL, -c $CTX, Vulkan device $VKDEV, -ram=$RAM)"
echo "log    $OUT"
echo "This takes a while: a systemd boot, the model load through 9p and onto"
echo "the GPU, then generation. It ends when the guest powers off."
echo

"$DOOMV" -ng -gpu "-ram=$RAM" \
    -opensbi=build/linux/fw_jump.elf -kernel=build/linux/Image \
    -dtb=build/linux/ubuntu.dtb "-disk=$RUNIMG" \
    -drives= -shared=shared \
    -expect="doomv login:" "-input=$SCRIPT" > "$OUT" 2>&1 || true
WALL=$SECONDS

echo "--- the guest's GPU (vulkaninfo) ---"
strings "$OUT" | sed -n '/LLAMA-READY/,/VULKANINFO-DONE/p' | sed '/LLAMA-READY\|VULKANINFO-DONE/d' | grep -v '^progress:' || true
echo "--- llama.cpp's Vulkan backend ---"
strings "$OUT" | grep -E 'Available devices|^ *Vulkan[0-9]+:|ggml_vulkan' | head -20 || true
echo "--- what the model produced ---"
strings "$OUT" | sed -n '/^assistant$/,/LLAMA-RUN-DONE/p' | grep -vE 'LLAMA-RUN-DONE|llama_perf|^progress:|^[0-9.]+ [IWE] ' || true
echo "--- timing (guest clock, then this computer's) ---"
strings "$OUT" | grep -E 'prompt eval time|[^t] eval time|LLAMA-RUN-DONE' || true
echo "wall clock: $((WALL / 60)) min $((WALL % 60)) s, boot included"
echo "--- full log: $OUT ---"
