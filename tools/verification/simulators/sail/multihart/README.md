# sail_riscv_mh: Sail as a machine of several harts

The Sail RISC-V model is one hart. DoomV's `-harts=N` is a machine of N, and
DoomV is held to Sail, so this builds the reference for it: `sail_riscv_mh`,
N copies of the Sail model running over the one memory the Sail runtime
keeps.

```sh
# in WSL, as ../build.sh is
bash tools/verification/simulators/sail/multihart/build.sh
build/cmake/c_emulator/sail_riscv_mh --harts 4 --config ../rva23s64.json <sail_riscv_sim's options> test.elf
```

## What is Sail's and what is added

The instruction set model is Sail's, generated from the submodule's sources
unchanged except for one platform device. What the model cannot say, because
it has one hart, the driver ([`sail_riscv_mh.cpp`](sail_riscv_mh.cpp)) says:

| | Where | What |
|---|---|---|
| Who runs when | driver | Round-robin, hart 0 first, one `try_step` each per round. DoomV's `-harts=N` runs the same order. |
| The clock | driver | One `mtime` for the machine. It ticks as `riscv_sim.cpp`'s loop ticks for one hart, counted in rounds: every `instructions_per_tick` rounds in which some hart was not waiting, and every round in which all harts are waiting. A guest write to `mtime` is copied to every model. |
| Hart ids | driver | Each model is initialised with the configuration's `platform.hartid` set to its number; nothing else in the configuration changes. Each starts at the ELF's entry with `a0` = its id, as Sail's `init_boot_requirements` sets. |
| Reservations | driver | A write by one hart (`mem_write_callback`) cancels every other hart's reservation on the bytes written, by the configured reservation set. A hart's own writes are left to the model, which follows `invalidate_on_same_hart_store`. |
| The CLINT array | [`multihart.patch`](multihart.patch) | Sail's CLINT decodes only the stepping hart's own `msip` and `mtimecmp`; any other hart's is an access fault. The patch adds three externs (`clint_remote_claim/read/write`) consulted only for an address the CLINT would otherwise fault on, and the driver serves them from the other model's own `msip` and `mtimecmp`. With one hart nothing is claimed, and the model is exactly Sail's. |

The patch also gives `ModelImpl` two accessors the driver needs (the model's
state, and its platform side) and adds the `sail_riscv_mh` target to CMake.

## The trace

With more than one hart, the driver writes `hart <i>` before each step of a
hart other than the one that stepped last. Everything up to the next such
line is about that hart: its instruction lines, effects and traps, and the
`mip` changes Sail's `update_mip` logs as a step begins. A record can span
another hart's: a `wfi`'s instruction line is written when the wait starts,
its trap (if it times out with TW set) when it ends. DoomV's `-lockstep`
reader keeps a record in progress per hart for this, and its `-trace` writes
the same markers.

## Building

`build.sh` exports the submodule's committed sources (`git archive`, with
LF line endings) to `build/src`, applies the patch there and copies the
driver in, so the submodule's own tree is never touched. The export is redone
only when the submodule's commit or the patch changes, since a fresh tree
regenerates and recompiles the whole model -- about as long as `../build.sh`.
It needs the same Sail compiler, in `/root/build/sail-bin`.

## Tests

`tools/verification/lockstep_sail.py --multihart` builds
`tools/verification/tests/lockstep/multihart/*.S` once per hart count, runs
this driver for the trace and DoomV with `-harts=N -lockstep-strict` against
it. See the [README](../../../../../README.md#harts).

## Co-simulation: DoomV's devices as Sail's inputs

`--cosim LOG --cosim-start PC` (one hart) runs the model as the CPU of a
DoomV machine whose devices Sail does not have -- the Linux machine
`tools/verification/lockstep_linux.py` lock-steps. [`cosim.patch`](cosim.patch),
applied after `multihart.patch`, adds four externs to the model:
`cosim_claim/read/write`, consulted for an access the CLINT, the test
interrupt generator and HTIF do not decode, and `cosim_external`, OR'd into
the platform's external-interrupt inputs. The driver serves them from DoomV's
`-cosim-log`: each device load gets the value DoomV's device gave, each
device store is checked against DoomV's, in order, and the SEIP line and the
devices' writes to RAM are applied before the step DoomV's log names. On
first reaching PC -- DoomV's first instruction, after the restore program
corun.py builds -- it takes on DoomV's step count, mtime and tick phase, and
the mstatus, mepc, mcycle and minstret the restore program's own mret and
instructions left different. Without `--cosim` nothing is claimed and the
external input is zero: the model is unchanged. `make_cosim_patch.py`
regenerates the patch from its edits.
