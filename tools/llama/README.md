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
bash tools/llama/run.sh shared/Qwen3.5-0.8B-UD-IQ2_XXS.gguf
bash tools/llama/run.sh shared/model.gguf "Once upon a time" 32
RAM=8G bash tools/llama/run.sh shared/bigger.gguf
```

Takes any GGUF, an optional prompt and an optional token count; `RAM=` and
`IMAGE=` override the guest size and the disk image. It boots Ubuntu, mounts
the share, copies the binary in, runs the model, powers the machine off -- so
the run ends by itself -- then prints what the model generated, leaving the full
console log in `build/llama/`.

It generates the `-input` script rather than keeping one per model, because the
model name and the prompt are text typed into the guest's shell and so are part
of that script. It also works on a *copy* of the image: a boot writes to the
disk, and a throwaway run should not be what the real image has been through --
see the determinism note in [scripts/README.md](../../scripts/README.md).

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

## "Segmentation fault" loading the model

Give the guest more memory. This is the one failure worth knowing in advance,
because llama.cpp reports it as a crash rather than as running out of room.

Reproduced here on a 1G guest, which is what `boot.py ubuntu` gives you without
`--ram`:

```
W common_fit_params: failed to fit params to free device memory:
  was unable to fit model into system memory by reducing context, abort
EXIT-DEFAULTCTX=139
```

139 is 128+11, a SIGSEGV. llama.cpp notices it cannot fit, tries shrinking the
context on its own -- 163584 down to 4096 -- fails, and then segfaults instead
of exiting. Nothing in the kernel log: no OOM killer, no message naming memory.
Just a crash while loading.

Capping the context by hand does not save it. With `-c 2048` on the same 1G
guest it got as far as generating two words and then died the same way,
exit 139. The context is not the problem; the total is.

| guest RAM | session | model | result |
|---|---|---|---|
| 1G | console | 323MB IQ2_XXS | SIGSEGV (139) |
| 1G | console, `-c 2048` | 323MB | SIGSEGV (139), a little later |
| 4G | console | 323MB | generates |
| 4G | console | 532MB Q4_K_M | generates |
| 4G | openbox desktop | either | SIGSEGV (139) |

**A desktop changes the answer.** 4G is enough for either model on the console
and not enough once X, openbox and a terminal are resident -- they are there
before llama asks for anything, and that is what tips it over. The model size
is not what matters: the 532MB model runs on 4G headless, and the 323MB one
crashes on 4G under a desktop.

So `--ram 4G` for a console run, and **8G or more when running a model inside
a desktop session**. Guest RAM the model does not use costs nothing -- the
allocation is lazy, and even 16G starts in 0.39s -- so there is no reason to
be sparing. `run.sh` defaults to 4G because it boots the console, not a
desktop.

The 8G figure is the recommendation that follows from the rows above rather
than a measured one: what was measured is that 4G is enough without a desktop
and not enough with one.

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
* **RAM.** `-ram=4G` is comfortable for an 0.8B model on the console, and not
  enough for one inside a desktop session -- see above. Unused guest RAM costs
  nothing -- the allocation is lazy -- so size it for the model rather than
  sparingly. A bigger model needs a bigger `-ram=` and nothing else.
* **Vector.** This builds for plain `rv64gc`. The guest implements far more,
  vector included, so an RVV build of ggml is the obvious next thing to try;
  it has not been tried here.
