# spike (reference RISC-V ISA simulator)

`src/` is a git submodule of
[riscv-software-src/riscv-isa-sim](https://github.com/riscv-software-src/riscv-isa-sim),
pinned to commit `231e0d5` -- whatever was current when this was set up,
not a tagged release. Used as the golden reference model in
`tools/vtest/` (see `run_arch_test.py`): DoomV's own execution is
compared against spike's for every riscv-arch-test case.

Run `./build.sh` to produce `spike.exe` in this directory (gitignored
build output). Must build with MSYS2's own `/usr/bin/gcc`/`g++`, not
ucrt64/clangarm64/mingw -- those are cross-compiler-flavored toolchains
missing POSIX headers (`arpa/inet.h`) spike's `fesvr` layer needs;
MSYS2's own gcc is the one that actually emulates a POSIX host closely
enough.

## Local patches (see build.sh)

- **`addr_t` collision**: MSYS's `machine/types.h` defines its own
  `addr_t` (`char*`) behind the same `__addr_t_defined` guard macro
  spike's `fesvr/memif.h` uses for its own (`uint64_t`-based) `addr_t`.
  Whichever header wins the race decides every subsequent translation
  unit's `addr_t`, causing type errors that only show up in some `.cc`
  files depending on unrelated include-order differences. Fixed by
  patching the guard into `memif.h` first (reverted after building, so
  the submodule stays pinned clean) plus passing `-D__addr_t_defined
  -D_GNU_SOURCE` globally (the latter for `strnlen`/`readlink`, POSIX-
  visibility-gated under `-std=c++2a`).
- **Broken symlink**: `spike_dasm/spike_dasm_option_parser.cc` is a real
  symlink in the upstream repo (to `../fesvr/option_parser.cc`), but git
  can't check out a real symlink without Windows dev-mode/admin
  privileges -- it materializes as a one-line text file containing just
  the link target path, which obviously doesn't compile. Fixed by
  copying the real target file's content over it. Left in place (not
  reverted) since re-running `git checkout` on this one file would just
  recreate the same broken state on this filesystem -- expect the
  submodule to always show this one file as locally modified.

Configure itself printed warnings about Boost::ASIO/Boost::Regex not
being found -- harmless, spike degrades gracefully without them (no
hard configure failure at this particular commit, unlike some other
points in spike's history).
