# Counters, interrupts and state controls

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

These features connect instructions to system behavior: counters, pending interrupts and access to privileged state. A readable CSR alone does not prove that its associated hardware behavior exists.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [Zicntr — basic counters](#zicntr--basic-counters)
- [Zihpm — hardware performance counters](#zihpm--hardware-performance-counters)
- [Sstc — supervisor timer compare](#sstc--supervisor-timer-compare)
- [Smaia — advanced interrupt CSRs](#smaia--advanced-interrupt-csrs)
- [Ssaia — advanced interrupt CSRs](#ssaia--advanced-interrupt-csrs)
- [Sscofpmf — counter overflow and mode filtering](#sscofpmf--counter-overflow-and-mode-filtering)
- [Ssstateen and Smstateen — state enable hierarchy](#ssstateen-and-smstateen--state-enable-hierarchy)

## Zicntr — basic counters

These are CSR reads, not new opcodes. RDCYCLE/RDTIME/RDINSTRET are CSRRS aliases. All three effective values read Timer::mtime. The name instret is misleading as an exact retirement statistic here: DoomSystem also calls step_instructions on interrupt-entry and fault paths.

Implementation: [src/extensions/ext_zicntr.cpp](../../src/extensions/ext_zicntr.cpp), [src/extensions/ext_zicntr.hpp](../../src/extensions/ext_zicntr.hpp), [src/doom_system.cpp](../../src/doom_system.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `cycle` | `0xC00` | Read-only alias of mtime; no pipeline-cycle model. |
| `time` | `0xC01` | Read-only alias of mtime; rate interpreted using DT timebase-frequency. |
| `instret` | `0xC02` | Same mtime alias, not separately tracked successful retirement. |
| `mcounteren` | `0x306` | M-controlled lower-mode permission bits: CY=0, TM=1, IR=2. |
| `scounteren` | `0x106` | Additional U-mode permission gate, intersected with mcounteren. |

RV32 high-half CSR semantics are not implemented by the low-window counter helper. Machine counter addresses such as mcycle/minstret fall through to generic storage rather than forming these live aliases.

[Back to contents](#contents)

## Zihpm — hardware performance counters

No new instructions and no independent extension flag. No event stream increments these counters.

Implementation: [src/extensions/ext_zicntr.cpp](../../src/extensions/ext_zicntr.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `hpmcounter3` | `0xC03` | Read-only zero; permission bit 3 in mcounteren/scounteren. |
| `hpmcounter4` | `0xC04` | Read-only zero; permission bit 4 in mcounteren/scounteren. |
| `hpmcounter5` | `0xC05` | Read-only zero; permission bit 5 in mcounteren/scounteren. |
| `hpmcounter6` | `0xC06` | Read-only zero; permission bit 6 in mcounteren/scounteren. |
| `hpmcounter7` | `0xC07` | Read-only zero; permission bit 7 in mcounteren/scounteren. |
| `hpmcounter8` | `0xC08` | Read-only zero; permission bit 8 in mcounteren/scounteren. |
| `hpmcounter9` | `0xC09` | Read-only zero; permission bit 9 in mcounteren/scounteren. |
| `hpmcounter10` | `0xC0A` | Read-only zero; permission bit 10 in mcounteren/scounteren. |
| `hpmcounter11` | `0xC0B` | Read-only zero; permission bit 11 in mcounteren/scounteren. |
| `hpmcounter12` | `0xC0C` | Read-only zero; permission bit 12 in mcounteren/scounteren. |
| `hpmcounter13` | `0xC0D` | Read-only zero; permission bit 13 in mcounteren/scounteren. |
| `hpmcounter14` | `0xC0E` | Read-only zero; permission bit 14 in mcounteren/scounteren. |
| `hpmcounter15` | `0xC0F` | Read-only zero; permission bit 15 in mcounteren/scounteren. |
| `hpmcounter16` | `0xC10` | Read-only zero; permission bit 16 in mcounteren/scounteren. |
| `hpmcounter17` | `0xC11` | Read-only zero; permission bit 17 in mcounteren/scounteren. |
| `hpmcounter18` | `0xC12` | Read-only zero; permission bit 18 in mcounteren/scounteren. |
| `hpmcounter19` | `0xC13` | Read-only zero; permission bit 19 in mcounteren/scounteren. |
| `hpmcounter20` | `0xC14` | Read-only zero; permission bit 20 in mcounteren/scounteren. |
| `hpmcounter21` | `0xC15` | Read-only zero; permission bit 21 in mcounteren/scounteren. |
| `hpmcounter22` | `0xC16` | Read-only zero; permission bit 22 in mcounteren/scounteren. |
| `hpmcounter23` | `0xC17` | Read-only zero; permission bit 23 in mcounteren/scounteren. |
| `hpmcounter24` | `0xC18` | Read-only zero; permission bit 24 in mcounteren/scounteren. |
| `hpmcounter25` | `0xC19` | Read-only zero; permission bit 25 in mcounteren/scounteren. |
| `hpmcounter26` | `0xC1A` | Read-only zero; permission bit 26 in mcounteren/scounteren. |
| `hpmcounter27` | `0xC1B` | Read-only zero; permission bit 27 in mcounteren/scounteren. |
| `hpmcounter28` | `0xC1C` | Read-only zero; permission bit 28 in mcounteren/scounteren. |
| `hpmcounter29` | `0xC1D` | Read-only zero; permission bit 29 in mcounteren/scounteren. |
| `hpmcounter30` | `0xC1E` | Read-only zero; permission bit 30 in mcounteren/scounteren. |
| `hpmcounter31` | `0xC1F` | Read-only zero; permission bit 31 in mcounteren/scounteren. |

Machine event-selector state is discussed under Sscofpmf. A generic writable machine CSR is not an implemented event counter.

[Back to contents](#contents)

## Sstc — supervisor timer compare

No new instruction and no independent configuration flag. Sstc allows S-mode to program a timer deadline without an SBI call for each rearm. When STCE is enabled, mtime >= stimecmp contributes STIP. The current helper supports RV64 compare state and does not implement the full privilege/virtual timer-control matrix.

Implementation: [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp), [src/timer.cpp](../../src/timer.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `stimecmp` | `0x14D` | Supervisor deadline compared against live mtime. |
| `menvcfg.STCE` | `0x30A bit 63` | Enables the modeled supervisor timer comparison. |

`stimecmph`, `vstimecmp` and its high-half counterpart are not implemented as live timer aliases. `mtime/mtimecmp` are MMIO registers, not Sstc CSRs.

[Back to contents](#contents)

## Smaia — advanced interrupt CSRs

No new opcode. This is the implemented AIA CSR subset, backed by the platform IMSIC objects. Top local interrupt and top external identity are different levels of arbitration. Programmable priority arrays, virtual AIA register banks and a full AIA implementation are not supplied merely by these windows.

Implementation: [src/imsic.cpp](../../src/imsic.cpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `miselect` | `0x350` | Select M-file indirect IMSIC register. |
| `mireg` | `0x351` | Read/write the selected M IMSIC register. |
| `mtopei` | `0x35C` | Peek top external interrupt; actual CSR write claims it. |
| `mtopi` | `0xFB0` | Read-only top pending enabled M interrupt, combining local causes and external delivery. |

[Back to contents](#contents)

## Ssaia — advanced interrupt CSRs

No new opcode. This is the implemented AIA CSR subset, backed by the platform IMSIC objects. Top local interrupt and top external identity are different levels of arbitration. Programmable priority arrays, virtual AIA register banks and a full AIA implementation are not supplied merely by these windows.

Implementation: [src/imsic.cpp](../../src/imsic.cpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `siselect` | `0x150` | Select S-file indirect IMSIC register. |
| `sireg` | `0x151` | Read/write selected S IMSIC register. |
| `stopei` | `0x15C` | Peek top S external interrupt; write claims it. |
| `stopi` | `0xDB0` | Read-only top pending enabled delegated interrupt; required for the advertised Linux AIA handler path. |

[Back to contents](#contents)

## Sscofpmf — counter overflow and mode filtering

CSR-only extension. The helper computes scountovf and exposes event-filter fields. No event source increments hpmcounter values. Crucially, lcofi_pending exists but the inspected compute_mip does not call it or propagate the LCOFIP shadow bit. Thus source comments describing complete interrupt plumbing overstate this snapshot.

Implementation: [src/extensions/ext_sscofpmf.cpp](../../src/extensions/ext_sscofpmf.cpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `scountovf` | `0xDA0` | Read-only bitmap of mhpmeventN.OF, filtered by counter permissions. |
| `mhpmevent3` | `0x323` | Event selector 3; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent4` | `0x324` | Event selector 4; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent5` | `0x325` | Event selector 5; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent6` | `0x326` | Event selector 6; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent7` | `0x327` | Event selector 7; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent8` | `0x328` | Event selector 8; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent9` | `0x329` | Event selector 9; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent10` | `0x32A` | Event selector 10; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent11` | `0x32B` | Event selector 11; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent12` | `0x32C` | Event selector 12; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent13` | `0x32D` | Event selector 13; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent14` | `0x32E` | Event selector 14; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent15` | `0x32F` | Event selector 15; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent16` | `0x330` | Event selector 16; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent17` | `0x331` | Event selector 17; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent18` | `0x332` | Event selector 18; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent19` | `0x333` | Event selector 19; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent20` | `0x334` | Event selector 20; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent21` | `0x335` | Event selector 21; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent22` | `0x336` | Event selector 22; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent23` | `0x337` | Event selector 23; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent24` | `0x338` | Event selector 24; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent25` | `0x339` | Event selector 25; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent26` | `0x33A` | Event selector 26; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent27` | `0x33B` | Event selector 27; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent28` | `0x33C` | Event selector 28; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent29` | `0x33D` | Event selector 29; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent30` | `0x33E` | Event selector 30; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |
| `mhpmevent31` | `0x33F` | Event selector 31; OF[63], MINH[62], SINH[61], UINH[60], VSINH[59], VUINH[58] retained. No live event counting. |

Architectural LCOFI uses mie/mip bit 13. Do not claim functioning overflow interrupts or programmable event counting from the presence of these fields. RV32 high-half event-selector semantics are not separately implemented.

[Back to contents](#contents)

## Ssstateen and Smstateen — state enable hierarchy

The repository exposes the SSSTATEEN flag and ssstateen token for its machine/supervisor/hypervisor state-enable helpers; smstateen is not an independently parsed token. State-enable controls allow privileged software to deny state it cannot save across context switches. The implemented helper gates access to lower stateen CSRs through SE0. ENVCFG is advertised writable, but ordinary envcfg access is not comprehensively gated by it in csr_access_permitted. State-enable access denial follows the existing CSR trap path rather than a complete virtual-instruction matrix.

Implementation: [src/extensions/ext_ssstateen.cpp](../../src/extensions/ext_ssstateen.cpp), [src/extensions/ext_ssstateen.hpp](../../src/extensions/ext_ssstateen.hpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `mstateen0` | `0x30C` | Writable SE0[63] and ENVCFG[62]. |
| `mstateen1` | `0x30D` | Read-only-zero fields under this implementation mask. |
| `mstateen2` | `0x30E` | Read-only-zero fields under this implementation mask. |
| `mstateen3` | `0x30F` | Read-only-zero fields under this implementation mask. |
| `sstateen0` | `0x10C` | Read-only-zero fields under this implementation mask. |
| `sstateen1` | `0x10D` | Read-only-zero fields under this implementation mask. |
| `sstateen2` | `0x10E` | Read-only-zero fields under this implementation mask. |
| `sstateen3` | `0x10F` | Read-only-zero fields under this implementation mask. |
| `hstateen0` | `0x60C` | Read-only-zero fields under this implementation mask. |
| `hstateen1` | `0x60D` | Read-only-zero fields under this implementation mask. |
| `hstateen2` | `0x60E` | Read-only-zero fields under this implementation mask. |
| `hstateen3` | `0x60F` | Read-only-zero fields under this implementation mask. |

No new instruction. RV32 high-half stateen CSRs are not separately modeled.

[Back to contents](#contents)
