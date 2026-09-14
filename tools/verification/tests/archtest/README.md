# riscv-arch-test against DoomV

The official RISC-V architectural test suite, run against DoomV with **Sail
as the reference model**.

This sits beside `../differential/vector`, and the two answer different
questions. The differential suites are hand-written: they cover what I
thought to test, which is exactly their limitation as evidence. This suite is
the architecture's own certification tests, and its pass criterion is
literally "produces the same signature as the reference model" -- which is
what makes a claim like *matches Sail* mean anything.

At profile level there is no alternative to Sail: riscv-arch-test ships an
RVA23S64 configuration for Sail only. There is no spike RVA23S64 config in
the repository at all.

## Running it

```sh
./setup.sh                        # once: toolchain, Sail 0.13.1, act, UDB
./gen_reference.sh Zicond         # compile tests + Sail reference signatures
python archtest.py Zicond         # run DoomV over the same ELFs and diff
```

Omit the extension argument for the whole suite. `gen_reference.sh` is the
slow half (it runs Sail over every test); `archtest.py` can be re-run freely
against an existing work tree.

## Things that are not obvious

**A timeout is the normal ending.** DoomV writes its signature when it
reaches the `-break` address and then keeps its SDL window open rather than
exiting, so every run has to be killed. The verdict comes from whether
`signature.log` appeared, never from the exit status. Reading the timeout as
the result reports every passing test as a failure.

**The two harnesses share one output path.** DoomV hardcodes `signature.log`
relative to its own working directory, so this suite takes the same
`.signature.lock` as `run_diff.sh` and holds it across the *move*, not just
the run. Releasing before claiming the file leaves a window where another
run's output gets attributed to this test -- which reads as a wrong answer
rather than as a collision.

**Signature widths differ.** Sail writes 64-bit words, DoomV writes 32-bit.
`archtest.py` recombines DoomV's pairs into doublewords before comparing, so
both sides are diffed at the same width; comparing at different widths is how
a byte-order bug hides.

## Deviations from the framework's declared environment

Both are deliberate, and both are the kind of thing that produces phantom
mismatches if left implicit.

**Sail is pinned to 0.13.1.** ACT4 checks the reference model version exactly
and refuses anything else; the 4.0.0 tag wanted 0.10, the `act4` branch wants
0.13.1. That build lives at `/root/build/sail-0131`, separate from the 0.14
the differential harness uses. The pin is what keeps a mismatch attributable
to DoomV instead of to a framework/model pairing artifact.

**GCC is newlib 14.2.0, where ACT4 asks for 15 or later.** Ubuntu packages no
newer newlib toolchain. The floor is lowered in `setup.sh`, in the open.

The tempting shortcut here -- pointing the framework at
`riscv64-linux-gnu-gcc` 15.2.0, which *does* satisfy the version check -- is
wrong, and not subtly. The tests compile, and then Sail's own reference run
dies in a trap loop before reaching the first test case. Verified both ways
on Zicond:

| compiler | Sail reference run |
| --- | --- |
| newlib `riscv64-unknown-elf-gcc` 14.2.0 | SUCCESS |
| `riscv64-linux-gnu-gcc` 15.2.0 | trap loop at `rvtest_boot_to_smode` |

**Symlinks are materialised.** The checkout is on a Windows filesystem with
`core.symlinks` false, so each of the 65 files git records as a symlink
arrived as a text file containing its target's path -- and the assembler
reports `unknown pseudo-op: '..'` when it tries to assemble one. `setup.sh`
copies each target's contents over its pointer, which works from both sides
of the WSL boundary in a way a real symlink would not.
