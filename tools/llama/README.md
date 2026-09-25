# llama.cpp on DoomV

Running a language model inside the guest. It works, it is slow, and the
slowness is the emulator rather than anything to fix: a 0.8B model at IQ2_XXS
generated 12 tokens in about 35 minutes of wall clock, boot and model load
included.

Everything here is the specific case of
[Running your own programs in the guest](../../README.md#own-programs).

## Build

```sh
bash tools/llama/build.sh
```

Clones llama.cpp in WSL, cross-compiles it for riscv64 Linux, and writes one
file: `shared/llama`, a ~15 MB stripped static binary. First run also installs
`g++-riscv64-linux-gnu`, which is a separate package from the C cross-compiler
this project already uses, and takes a few minutes. `LLAMA_REF=<sha>` builds a
particular commit instead of master.

The flags that matter, and why, are in the script: static (the guest's glibc is
not the build machine's), `GGML_OPENMP=OFF` (libgomp has no static cross build
here and the link fails on it), `GGML_NATIVE=OFF` (the build machine is x86),
`LLAMA_CURL=OFF`.

## Model

Put a GGUF in `shared/`:

```
shared/Qwen3.5-0.8B-UD-IQ2_XXS.gguf
```

It stays on the share and is read in place -- it does not need copying into the
guest, and at hundreds of megabytes you would not want to. Nothing about the
choice is special; any GGUF llama.cpp can load will do, and a less aggressive
quantisation than IQ2_XXS will talk more sense.

## Run it unattended

```sh
cp ubuntu.img build/llama-ubuntu.img

./riscv_doom.exe -ng -ram=4G \
  -opensbi=build/linux/fw_jump.elf -kernel=build/linux/Image \
  -dtb=build/linux/ubuntu.dtb -disk=build/llama-ubuntu.img \
  -drives= -shared=shared \
  -expect="doomv login:" -input=tools/llama/run-qwen.script \
  | tee build/llama-run.log
```

`run-qwen.script` logs in, mounts the share, copies the binary in, runs the
model and powers the machine off, so the emulator exits on its own. Edit the
model name and the prompt on its `completion` line.

On a copy of the image, because a run writes to the disk and the copy is what
keeps the next run comparable -- see the determinism note in
[scripts/README.md](../../scripts/README.md).

## Run it by hand

Boot Ubuntu normally with room for the model:

```sh
python scripts/boot.py ubuntu --ram 4G
```

Then in the guest:

```sh
mkdir -p /mnt/shared
mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared
cp /mnt/shared/llama /root/ && chmod +x /root/llama          # the share carries no execute bit
/root/llama version
/root/llama completion -m /mnt/shared/Qwen3.5-0.8B-UD-IQ2_XXS.gguf \
    -p "The capital of France is" -n 12 -t 1 --no-warmup
```

`-t 1` because the machine has one hart; more threads only adds contention.

## What it looks like when it works

```
version: 0.5.0-dev (build 1, commit 4b1a27f)
generate: n_ctx = 163584, n_batch = 2048, n_predict = 12, n_keep = 0

user
The capital of France is
assistant
<think>
</think>
The capital of France is the capital of
```

## Notes

* **`llama-cli` does not exist any more.** Upstream folded the CLI into one
  `llama` binary with subcommands, so it is `llama completion ...`, and the
  CMake target to build is `llama-app`. `llama help` lists the rest.
* **`Permission denied` running from `/mnt/shared`** is the execute bit, not
  the binary. Windows has none, so everything on the share arrives
  `-rw-rw-rw-`; copy it into the guest and `chmod +x`.
* **Timings the model prints are about the emulated machine.** The guest clock
  comes from the instruction count, so tokens per second there is a statement
  about DoomV, not your CPU.
* **RAM.** `-ram=4G` is comfortable for an 0.8B model. Unused guest RAM costs
  nothing -- the allocation is lazy -- so size it for the model rather than
  sparingly. A bigger model needs a bigger `-ram=` and nothing else.
* **Vector.** This builds for plain `rv64gc`. The guest implements far more,
  vector included, so an RVV build of ggml is the obvious next thing to try;
  it has not been tried here.
