# DoomV ISA guide and reference

[Documentation home](README.md) · [Boot walkthrough](BOOT_FLOW.md) · [Architecture](DEVICES_AND_ARCHITECTURE.md)

Use this page to understand configuration and choose an instruction family.
The detailed tables live in focused chapters. Existing section links below
still lead to the corresponding material.

## Read by task

| You are working on… | Reference |
|---|---|
| Arithmetic, branches, atomics or compact encodings | [Integer instructions](isa/integer.md) |
| Rounding, NaNs or exception flags | [Scalar floating point](isa/floating-point.md) |
| Fences, hints or cache-block operations | [Hints and ordering](isa/hints-and-cache.md) |
| Vector length, element width, loads or stores | [Vector configuration and memory](isa/vector-memory.md) |
| Vector calculations, masks or rearranging elements | [Vector arithmetic](isa/vector-arithmetic.md) |
| Trap entry, CSR permissions, page tables or PMP | [Privilege and memory protection](isa/privileged.md) |
| Guest CSRs or two-stage translation | [Hypervisor support](isa/hypervisor.md) |
| Timers, pending interrupts or state controls | [System state](isa/system-state.md) |

## How to read an entry

Read the family explanation, then its implementation links. Use the table
for individual operations and the CSR section for state changes. Mnemonics
describe architectural operations; support limits describe this interpreter.

`rd` is a destination register; `rs1` and `rs2` are source registers. `XLEN`
is the integer register width. A suffix `.w` or `.d` has context-dependent
meaning: integer word operations and floating-point formats are different
uses of similar notation.

For any instruction, check four things: whether decoding permits it, which
state it reads, which state it changes, and how faults are reported. A useful
test checks all four rather than only the final register value.

## Contents

- [Configuration and scope](#configuration-and-scope)
- [I — RV32I and RV64I](#i--rv32i-and-rv64i)
- [M — integer multiplication and division](#m--integer-multiplication-and-division)
- [A — atomics and reservations](#a--atomics-and-reservations)
- [C — compressed instructions](#c--compressed-instructions)
- [Zcb — additional compressed operations](#zcb--additional-compressed-operations)
- [F — single precision floating point](#f--single-precision-floating-point)
- [D — double precision floating point](#d--double-precision-floating-point)
- [Zfa — additional floating point](#zfa--additional-floating-point)
- [Zfhmin — minimal scalar half precision](#zfhmin--minimal-scalar-half-precision)
- [Zba — address generation](#zba--address-generation)
- [Zbb — basic bit manipulation](#zbb--basic-bit-manipulation)
- [Zbs — single-bit manipulation](#zbs--single-bit-manipulation)
- [Zicond — conditional zero](#zicond--conditional-zero)
- [Zicsr — CSR access instructions](#zicsr--csr-access-instructions)
- [Zifencei — instruction-fetch synchronization](#zifencei--instruction-fetch-synchronization)
- [Zihintpause — pause hint](#zihintpause--pause-hint)
- [Zihintntl — non-temporal locality hints](#zihintntl--non-temporal-locality-hints)
- [Zimop — may-be operations](#zimop--may-be-operations)
- [Zcmop — compressed may-be operations](#zcmop--compressed-may-be-operations)
- [Zicbom — cache-block management](#zicbom--cache-block-management)
- [Zicbop — prefetch hints](#zicbop--prefetch-hints)
- [Zicboz — block zero](#zicboz--block-zero)
- [Zawrs — wait on reservation set](#zawrs--wait-on-reservation-set)
- [Zicntr — basic counters](#zicntr--basic-counters)
- [Zihpm — hardware performance counters](#zihpm--hardware-performance-counters)
- [V — vector configuration and execution model](#v--vector-configuration-and-execution-model)
- [V — vector memory instructions](#v--vector-memory-instructions)
- [V — integer and fixed-point instructions](#v--integer-and-fixed-point-instructions)
- [V — permutations masks and reductions](#v--permutations-masks-and-reductions)
- [V — floating-point instructions](#v--floating-point-instructions)
- [Zvfhmin — minimal vector half conversion](#zvfhmin--minimal-vector-half-conversion)
- [Zvbb — vector bit manipulation](#zvbb--vector-bit-manipulation)
- [Machine privilege — traps and control](#machine-privilege--traps-and-control)
- [Supervisor privilege and Sv39](#supervisor-privilege-and-sv39)
- [Svinval — fine-grained translation invalidation](#svinval--fine-grained-translation-invalidation)
- [Svnapot — naturally aligned translation ranges](#svnapot--naturally-aligned-translation-ranges)
- [Svpbmt — page-based memory types](#svpbmt--page-based-memory-types)
- [Svade and Svadu — accessed and dirty policy](#svade-and-svadu--accessed-and-dirty-policy)
- [Smpmp — physical memory protection](#smpmp--physical-memory-protection)
- [Ssnpm — pointer masking](#ssnpm--pointer-masking)
- [Smnpm — pointer masking](#smnpm--pointer-masking)
- [Sspm — pointer masking](#sspm--pointer-masking)
- [H — hypervisor extension](#h--hypervisor-extension)
- [Sstc — supervisor timer compare](#sstc--supervisor-timer-compare)
- [Smaia — advanced interrupt CSRs](#smaia--advanced-interrupt-csrs)
- [Ssaia — advanced interrupt CSRs](#ssaia--advanced-interrupt-csrs)
- [Sscofpmf — counter overflow and mode filtering](#sscofpmf--counter-overflow-and-mode-filtering)
- [Ssstateen and Smstateen — state enable hierarchy](#ssstateen-and-smstateen--state-enable-hierarchy)
- [Profiles implied subsets and unsupported names](#profiles-implied-subsets-and-unsupported-names)
- [Validation and maintenance](#validation-and-maintenance)

## Configuration and scope

This reference documents the implementation and its limits. The detailed
inventory starts at `6b37ec0`; configuration and selected trap behavior were
rechecked at `c7d881b`. See the [review scope](README.md#reading-the-evidence).

**Privileged** behavior includes trap control, translation and protection.
**Unprivileged** instructions are generally available to applications, subject
to enabled state and permissions. S and U are privilege modes, not separate
arithmetic instruction sets.

The chapters retain instruction and CSR coverage from the original inventory.
Source links identify where behavior lives; a listed instruction is not a
claim that every corner case has passed an independent test.

`ExtensionConfig` is global and selected before the machine is constructed.
The command-line parser is permissive, so first check which launch path is in use:

| Launch choice | Result |
|---|---|
| DOOM without `-march` | Broad defaults; V and H start off |
| Linux without `-march` | `main.cpp` supplies an explicit extension string that includes V |
| Either workload with `-march` | Most switches reset, then recognized names enable features; I remains enabled |
| An unknown or profile-shaped string | No strict validation; acceptance does not establish profile support |

Some names imply additional state:

| Name | DoomV parser behavior |
|---|---|
| `g` | Enables I/M/A/F/D; spell out Zicsr and Zifencei separately in this implementation |
| `b` | Enables Zba/Zbb/Zbs |
| `d` | Also enables F |
| `v` | Also enables D and F |
| `zfa` | Also enables D and F |
| `zfh` or `zfhmin` | Enables the minimal half-precision feature and F, not full Zfh arithmetic |
| `zcmop` | Also enables Zimop |

For example, `-march=rv64imafdc_zicsr_zifencei` selects a scalar configuration.
Adding `v` to the base letters enables vector execution. Adding only `v` as
a separate arbitrary token does not make this a standards-compliant parser.
When changing the Linux configuration, also check what the device tree
advertises and what the userspace binary can execute.

Several features do not have independent switches. Smpmp retains its default;
S/U, counters, Sstc and AIA are not separately controlled by this parser.
Zcb follows C and the underlying arithmetic features. Zvbb and Zvfhmin follow V.

Decoded instructions are cached by PC and raw bytes. Live FS/VS checks follow the cache lookup. Arithmetic generally advances PC by 4, compressed aliases by 2; branches and traps override this. Sources sometimes contain historical comments contradicted by later code. The behavior notes below follow the executable paths.

Implementation: [extension defaults](../src/extensions.hpp),
[ISA parser](../src/extensions.cpp), [launch defaults](../src/main.cpp), and
[decoder](../src/riscv_decoder.cpp).

[Back to contents](#contents)

## I — RV32I and RV64I

[Read the explanation, instruction table and CSR effects](isa/integer.md#i--rv32i-and-rv64i).

## M — integer multiplication and division

[Read the explanation, instruction table and CSR effects](isa/integer.md#m--integer-multiplication-and-division).

## A — atomics and reservations

[Read the explanation, instruction table and CSR effects](isa/integer.md#a--atomics-and-reservations).

## C — compressed instructions

[Read the explanation, instruction table and CSR effects](isa/integer.md#c--compressed-instructions).

## Zcb — additional compressed operations

[Read the explanation, instruction table and CSR effects](isa/integer.md#zcb--additional-compressed-operations).

## F — single precision floating point

[Read the explanation, instruction table and CSR effects](isa/floating-point.md#f--single-precision-floating-point).

## D — double precision floating point

[Read the explanation, instruction table and CSR effects](isa/floating-point.md#d--double-precision-floating-point).

## Zfa — additional floating point

[Read the explanation, instruction table and CSR effects](isa/floating-point.md#zfa--additional-floating-point).

## Zfhmin — minimal scalar half precision

[Read the explanation, instruction table and CSR effects](isa/floating-point.md#zfhmin--minimal-scalar-half-precision).

## Zba — address generation

[Read the explanation, instruction table and CSR effects](isa/integer.md#zba--address-generation).

## Zbb — basic bit manipulation

[Read the explanation, instruction table and CSR effects](isa/integer.md#zbb--basic-bit-manipulation).

## Zbs — single-bit manipulation

[Read the explanation, instruction table and CSR effects](isa/integer.md#zbs--single-bit-manipulation).

## Zicond — conditional zero

[Read the explanation, instruction table and CSR effects](isa/integer.md#zicond--conditional-zero).

## Zicsr — CSR access instructions

[Read the explanation, instruction table and CSR effects](isa/privileged.md#zicsr--csr-access-instructions).

## Zifencei — instruction-fetch synchronization

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zifencei--instruction-fetch-synchronization).

## Zihintpause — pause hint

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zihintpause--pause-hint).

## Zihintntl — non-temporal locality hints

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zihintntl--non-temporal-locality-hints).

## Zimop — may-be operations

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zimop--may-be-operations).

## Zcmop — compressed may-be operations

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zcmop--compressed-may-be-operations).

## Zicbom — cache-block management

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zicbom--cache-block-management).

## Zicbop — prefetch hints

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zicbop--prefetch-hints).

## Zicboz — block zero

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zicboz--block-zero).

## Zawrs — wait on reservation set

[Read the explanation, instruction table and CSR effects](isa/hints-and-cache.md#zawrs--wait-on-reservation-set).

## Zicntr — basic counters

[Read the explanation, instruction table and CSR effects](isa/system-state.md#zicntr--basic-counters).

## Zihpm — hardware performance counters

[Read the explanation, instruction table and CSR effects](isa/system-state.md#zihpm--hardware-performance-counters).

## V — vector configuration and execution model

[Read the explanation, instruction table and CSR effects](isa/vector-memory.md#v--vector-configuration-and-execution-model).

## V — vector memory instructions

[Read the explanation, instruction table and CSR effects](isa/vector-memory.md#v--vector-memory-instructions).

## V — integer and fixed-point instructions

[Read the explanation, instruction table and CSR effects](isa/vector-arithmetic.md#v--integer-and-fixed-point-instructions).

## V — permutations masks and reductions

[Read the explanation, instruction table and CSR effects](isa/vector-arithmetic.md#v--permutations-masks-and-reductions).

## V — floating-point instructions

[Read the explanation, instruction table and CSR effects](isa/vector-arithmetic.md#v--floating-point-instructions).

## Zvfhmin — minimal vector half conversion

[Read the explanation, instruction table and CSR effects](isa/vector-arithmetic.md#zvfhmin--minimal-vector-half-conversion).

## Zvbb — vector bit manipulation

[Read the explanation, instruction table and CSR effects](isa/vector-arithmetic.md#zvbb--vector-bit-manipulation).

## Machine privilege — traps and control

[Read the explanation, instruction table and CSR effects](isa/privileged.md#machine-privilege--traps-and-control).

## Supervisor privilege and Sv39

[Read the explanation, instruction table and CSR effects](isa/privileged.md#supervisor-privilege-and-sv39).

## Svinval — fine-grained translation invalidation

[Read the explanation, instruction table and CSR effects](isa/privileged.md#svinval--fine-grained-translation-invalidation).

## Svnapot — naturally aligned translation ranges

[Read the explanation, instruction table and CSR effects](isa/privileged.md#svnapot--naturally-aligned-translation-ranges).

## Svpbmt — page-based memory types

[Read the explanation, instruction table and CSR effects](isa/privileged.md#svpbmt--page-based-memory-types).

## Svade and Svadu — accessed and dirty policy

[Read the explanation, instruction table and CSR effects](isa/privileged.md#svade-and-svadu--accessed-and-dirty-policy).

## Smpmp — physical memory protection

[Read the explanation, instruction table and CSR effects](isa/privileged.md#smpmp--physical-memory-protection).

## Ssnpm — pointer masking

[Read the explanation, instruction table and CSR effects](isa/privileged.md#ssnpm--pointer-masking).

## Smnpm — pointer masking

[Read the explanation, instruction table and CSR effects](isa/privileged.md#smnpm--pointer-masking).

## Sspm — pointer masking

[Read the explanation, instruction table and CSR effects](isa/privileged.md#sspm--pointer-masking).

## H — hypervisor extension

[Read the explanation, instruction table and CSR effects](isa/hypervisor.md#h--hypervisor-extension).

## Sstc — supervisor timer compare

[Read the explanation, instruction table and CSR effects](isa/system-state.md#sstc--supervisor-timer-compare).

## Smaia — advanced interrupt CSRs

[Read the explanation, instruction table and CSR effects](isa/system-state.md#smaia--advanced-interrupt-csrs).

## Ssaia — advanced interrupt CSRs

[Read the explanation, instruction table and CSR effects](isa/system-state.md#ssaia--advanced-interrupt-csrs).

## Sscofpmf — counter overflow and mode filtering

[Read the explanation, instruction table and CSR effects](isa/system-state.md#sscofpmf--counter-overflow-and-mode-filtering).

## Ssstateen and Smstateen — state enable hierarchy

[Read the explanation, instruction table and CSR effects](isa/system-state.md#ssstateen-and-smstateen--state-enable-hierarchy).

## Profiles implied subsets and unsupported names

RVA23 is a profile describing a required collection of ISA behavior and constraints; it is not synonymous with “the parser accepted my string.” Zic64b describes 64-byte cache blocks, not a new instruction. Zve*/Zvl* describe vector subsets/minimum register lengths, not separate implementations here. Zaamo/Zalrsc and Zca/Zcd/Zcf describe subsets of implemented families but have no independent switches. Zfh/Zvfh full arithmetic, Zbc carryless multiplication, scalar/vector crypto, Zacas, Ztso, Zfinx, Smepmp, Sv48 and Sv57 are not established implementations in this snapshot. Unknown tokens are silently ignored. This list illustrates the boundary; the document does not claim to catalog every ratified extension absent from DoomV.

Implementation: [src/extensions.cpp](../src/extensions.cpp), [src/extensions.hpp](../src/extensions.hpp).

### CSR effects

CSR presence via generic fallback must not be used as a feature-support test. A feature needs verified decode/execute semantics, CSR behavior, privilege rules and tests.

[Back to contents](#contents)

## Validation and maintenance

The repository contains architectural and differential harnesses, not a blanket guarantee. `docs/BUGS.md` includes historical counts and configuration differences; do not promote them to a fresh result for this revision. This documentation task ran no guest builds or conformance suites. Static checks cover documentation anchors, source links, extension flags and named scalar mnemonic coverage.

When adding an extension, update its parser/feature reporting, decoder, executor, CSR behavior, DT claims when relevant and this reference. Test enabled/disabled encodings, privilege failures, illegal fields, destination x0, rounding/flags, memory faults and restart behavior. For vectors compare execution cases as well as display mnemonics. For H compare virtual CSR aliases and first/second-stage faults independently.

Architectural terminology can be checked against the [RISC-V ISA manual](https://docs.riscv.org/reference/isa/v20260120/_attachments/riscv-unprivileged.pdf) and [supervisor architecture](https://docs.riscv.org/reference/isa/v20250508/priv/supervisor.html). The instruction tables here are grounded primarily in the linked source implementation; intended semantics and identified gaps are explicitly separated.

Implementation: [tools/verification/tests/differential/vector/compare.py](../tools/verification/tests/differential/vector/compare.py), [tools/verification/tests/archtest/archtest.py](../tools/verification/tests/archtest/archtest.py), [docs/BUGS.md](../docs/BUGS.md).

[Back to contents](#contents)
