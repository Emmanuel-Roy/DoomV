# OpenSBI (M-mode firmware)

`src/` is a git submodule of
[riscv-software-src/opensbi](https://github.com/riscv-software-src/opensbi),
pinned to tag `v1.3`. Run `./build.sh` to produce `fw_jump.elf`/
`fw_jump.bin` in this directory (both gitignored build output, same as
`tools/doombuild`'s guest `.elf`). `fw_jump` (not
`fw_dynamic`/`fw_payload`) was chosen because it's the simplest boot
model — a fixed, non-relocatable firmware image loaded at a known
address, which fits an emulator with a fully custom, always-the-same
memory map (no need for `fw_dynamic`'s relocation support, which is
aimed at real hardware where the load address varies by board).

Entry point: `0x80000000` (the generic platform's default RAM base --
not DoomV's own `RAM_BASE`; reconciling the two, or telling the
platform where DoomV's RAM/devices actually live, is Stage 3's job,
not done yet).

## Why v1.3, not the latest release

Current OpenSBI (post-v1.3) unconditionally requires a linker with
RISC-V PIE support (`Your linker does not support creating PIEs`) --
this project's available toolchains (`riscv-none-elf-gcc` and MSYS2's
`riscv64-unknown-elf-gcc`) both lack it, and a full `riscv64-*-linux-gnu`
toolchain (which reliably has PIE support, since Linux userspace needs
it) isn't available/practical to build from source in this environment.
v1.3 predates that requirement.

## Build

`build.sh` handles it end to end, including a one-line patch `sed`'d
into `src/Makefile` (not committed -- the submodule stays pinned to a
clean, unmodified upstream commit) that forces `-std=gnu11`: this is a
few-years-old codebase hitting GCC 15's new C23 default, under which
`bool`/`true`/`false` are real keywords -- OpenSBI's `sbi_types.h` still
does `typedef int bool` / `#define true 1` / `#define false 0`, which
collides with the new keywords. Forcing the older standard back is
simpler and more robust than patching every resulting type mismatch
individually. `PLATFORM_RISCV_ISA` needs `zifencei` explicit too -- the
default march string this OpenSBI version picks doesn't include it, and
`sbi_tlb.c` uses `fence.i` directly.
