# DoomV ISA extension and CSR reference

Source baseline: `6b37ec0675cb052e4821c227a7b4c57af89b280f` (main snapshot inspected for this documentation).

Companion guides: [booting](BOOT_FLOW.md) · [devices and architecture](DEVICES_AND_ARCHITECTURE.md).

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

This reference describes the **DoomV source snapshot**, not a certification of every instruction or every ratified RISC-V extension. It covers every extension flag, execution family, CSR-only feature and platform ISA claim found in the superproject. Privileged means M/S/HS/VS control and memory protection; unprivileged means instructions applications can normally execute subject to enabled state and access permissions. S/U are privilege modes, not synonyms for two sets of arithmetic instructions.

The reviewed base is `6b37ec0675cb052e4821c227a7b4c57af89b280f`. The full superproject tree was inventoried; first-party source, build scripts, guest platform code, test assembly, harnesses and documentation were collected. Vendored SDL headers/libraries and guest binaries were inventoried as dependencies, not treated as DoomV implementations. Submodules were recorded at their pinned commits; selected OpenSBI, Linux and doomgeneric entry files were inspected. This is not a claim to have line-reviewed the entire Linux kernel or every external simulator.

`ExtensionConfig` is global and configured before construction. No `-march` keeps the broad defaults: V/H off, most other named flags on. An explicit string resets most switches, always retains I, and enables recognized strings. This is permissive substring parsing, not an ISA/profile validator. `g` enables I/M/A/F/D here but does **not** itself enable Zicsr/Zifencei; spell those out. `b` enables Zba/Zbb/Zbs. D implies F, V implies D/F, Zfa forces D/F, and Zfhmin implies F. A `zfh` token only enables Zfhmin. Smpmp is not reset or parsed and remains enabled by default. S/U, Zicntr/Zihpm, Sstc and AIA have no independent switches. Zcb rides C and its underlying arithmetic flags; Zvbb and Zvfhmin ride V. Thus neither `-march=rva23s64` nor arbitrary ratified names constitute a supported profile preset.

Decoded instructions are cached by PC and raw bytes. Live FS/VS checks follow the cache lookup. Arithmetic generally advances PC by 4, compressed aliases by 2; branches and traps override this. Sources sometimes contain historical comments contradicted by later code. The behavior notes below follow the executable paths.

Implementation: [src/extensions.hpp](../src/extensions.hpp), [src/extensions.cpp](../src/extensions.cpp), [src/riscv_decoder.cpp](../src/riscv_decoder.cpp).

### CSR effects

No new CSRs.

[Back to contents](#contents)

## I — RV32I and RV64I

I supplies control flow, integer computation and memory access. x0 discards writes; other registers are 64-bit host containers even in RV32 mode. Loads/stores call translation and physical-access checking. Ordinary scalar misalignment is handled in software paths rather than modeling a hardware pipeline. ECALL/EBREAK belong to the base architecture but DoomV routes their SYSTEM encodings through its Zicsr execution family, so that switch matters in this implementation.

Implementation: [src/extensions/ext_i.cpp](../src/extensions/ext_i.cpp), [src/registers.cpp](../src/registers.cpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `lui` | Place the U-immediate in bits 31:12; RV64 sign-extends the 32-bit result. |
| `auipc` | Add the sign-extended U-immediate to the address of this instruction. |
| `jal` | Write the next PC to rd and jump to PC plus the signed offset; rd=x0 discards the link. |
| `jalr` | Write the next PC to rd; jump to rs1 plus signed immediate with target bit 0 cleared. |
| `beq` | Branch if the two integer operands are equal. |
| `bne` | Branch if the operands differ. |
| `blt` | Branch on signed less-than. |
| `bge` | Branch on signed greater-than-or-equal. |
| `bltu` | Branch on unsigned less-than. |
| `bgeu` | Branch on unsigned greater-than-or-equal. |
| `addi` | Add a signed immediate, wrapping at XLEN. |
| `slti` | Set rd to 1 for signed rs1 < signed immediate, otherwise 0. |
| `sltiu` | Compare unsigned rs1 against the XLEN representation of the sign-extended immediate. |
| `xori` | Bitwise XOR with sign-extended immediate. |
| `ori` | Bitwise OR with sign-extended immediate. |
| `andi` | Bitwise AND with sign-extended immediate. |
| `slli` | Logical left shift by immediate; the permitted shift width depends on XLEN. |
| `srli` | Logical right shift, filling high bits with zero. |
| `srai` | Arithmetic right shift, replicating the sign bit. |
| `add` | Add rs1 and rs2 modulo 2^XLEN. |
| `sub` | Subtract rs2 from rs1 modulo 2^XLEN. |
| `sll` | Left shift by the low log2(XLEN) bits of rs2. |
| `slt` | Signed less-than comparison, returning 0 or 1. |
| `sltu` | Unsigned less-than comparison, returning 0 or 1. |
| `xor` | Bitwise exclusive OR. |
| `srl` | Logical right shift by the masked register shift count. |
| `sra` | Arithmetic right shift by the masked register shift count. |
| `or` | Bitwise inclusive OR. |
| `and` | Bitwise AND. |
| `fence` | Order selected predecessor and successor memory/I/O operations; the single-hart synchronous model needs no queue drain. |
| `ecall` | Request an execution-environment service by taking a trap; cause is 8 from U, 9 from S/HS, 10 from VS, 11 from M. It does not itself implement a syscall. |
| `ebreak` | Take a breakpoint exception, cause 3; separate from the host debugger PC-breakpoint mechanism. |
| `lb` | Load 8 bits from rs1 + immediate; sign-extend to XLEN. |
| `lh` | Load 16 bits from rs1 + immediate; sign-extend to XLEN. |
| `lw` | Load 32 bits from rs1 + immediate; sign-extend to XLEN. |
| `lbu` | Load 8 bits from rs1 + immediate; zero-extend to XLEN. |
| `lhu` | Load 16 bits from rs1 + immediate; zero-extend to XLEN. |
| `lwu` | Load 32 bits from rs1 + immediate; zero-extend to XLEN. RV64 only. |
| `ld` | Load 64 bits from rs1 + immediate; preserve all bits. RV64 only. |
| `sb` | Store the low 8 bits of rs2 to rs1 + immediate. |
| `sh` | Store the low 16 bits of rs2 to rs1 + immediate. |
| `sw` | Store the low 32 bits of rs2 to rs1 + immediate. |
| `sd` | Store the low 64 bits of rs2 to rs1 + immediate. RV64 only. |
| `addiw` | RV64 only: add immediate in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `addw` | RV64 only: add registers in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `subw` | RV64 only: subtract registers in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `slliw` | RV64 only: left shift by immediate in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `srliw` | RV64 only: logical right shift by immediate in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `sraiw` | RV64 only: arithmetic right shift by immediate in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `sllw` | RV64 only: left shift by register in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `srlw` | RV64 only: logical right shift by register in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |
| `sraw` | RV64 only: arithmetic right shift by register in 32 bits, then sign-extend to 64; word shifts use a 5-bit count. |

### CSR effects

No dedicated new arithmetic CSR. ECALL/EBREAK update the trap CSRs described under machine/supervisor architecture.

[Back to contents](#contents)

## M — integer multiplication and division

Integer products and quotients use explicit widths and signedness. Division by zero yields an all-ones quotient and the dividend as remainder; signed minimum / -1 yields the minimum and remainder zero. These cases must avoid host C++ division undefined behavior.

Implementation: [src/extensions/ext_m.cpp](../src/extensions/ext_m.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `mul` | Low XLEN bits of the product. |
| `mulh` | High XLEN bits of signed × signed product. |
| `mulhsu` | High XLEN bits of signed rs1 × unsigned rs2 product. |
| `mulhu` | High XLEN bits of unsigned × unsigned product. |
| `div` | Signed quotient, truncating toward zero. |
| `divu` | Unsigned quotient. |
| `rem` | Signed remainder with the dividend's sign when nonzero. |
| `remu` | Unsigned remainder. |
| `mulw` | RV64: low 32-bit product, sign-extended. |
| `divw` | RV64: signed 32-bit quotient, sign-extended. |
| `divuw` | RV64: unsigned 32-bit quotient, sign-extended even when bit 31 is set. |
| `remw` | RV64: signed 32-bit remainder, sign-extended. |
| `remuw` | RV64: unsigned 32-bit remainder, sign-extended. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## A — atomics and reservations

A supplies LR/SC and AMOs. `.aq`, `.rl` and `.aqrl` qualify ordering rather than changing the arithmetic; the interpreter is single-hart and synchronous. Natural alignment is checked and faults are distinct from translation failures. AMO permission needs both read and write. The reservation is internal RiscvCore state, not a CSR and not proof of multi-hart coherence. Zaamo/Zalrsc are useful architectural subset names, but the parser only exposes A here.

Implementation: [src/extensions/ext_a.cpp](../src/extensions/ext_a.cpp), [src/riscv_core.hpp](../src/riscv_core.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `lr.w` | 32-bit operation. Load the old memory value and establish the core reservation. Narrow loaded results are sign-extended. |
| `sc.w` | 32-bit operation. Store rs2 only if the reservation succeeds; rd=0 on success and nonzero on failure; consume the reservation. |
| `amoswap.w` | 32-bit operation. Replace memory with rs2. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amoadd.w` | 32-bit operation. Add rs2 to memory modulo the memory width. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amoxor.w` | 32-bit operation. XOR memory with rs2. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amoand.w` | 32-bit operation. AND memory with rs2. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amoor.w` | 32-bit operation. OR memory with rs2. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amomin.w` | 32-bit operation. Store the smaller signed operand. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amomax.w` | 32-bit operation. Store the larger signed operand. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amominu.w` | 32-bit operation. Store the smaller unsigned operand. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `amomaxu.w` | 32-bit operation. Store the larger unsigned operand. AMO returns the old memory value in rd. Narrow loaded results are sign-extended. |
| `lr.d` | 64-bit operation. Load the old memory value and establish the core reservation. RV64 only. |
| `sc.d` | 64-bit operation. Store rs2 only if the reservation succeeds; rd=0 on success and nonzero on failure; consume the reservation. RV64 only. |
| `amoswap.d` | 64-bit operation. Replace memory with rs2. AMO returns the old memory value in rd. RV64 only. |
| `amoadd.d` | 64-bit operation. Add rs2 to memory modulo the memory width. AMO returns the old memory value in rd. RV64 only. |
| `amoxor.d` | 64-bit operation. XOR memory with rs2. AMO returns the old memory value in rd. RV64 only. |
| `amoand.d` | 64-bit operation. AND memory with rs2. AMO returns the old memory value in rd. RV64 only. |
| `amoor.d` | 64-bit operation. OR memory with rs2. AMO returns the old memory value in rd. RV64 only. |
| `amomin.d` | 64-bit operation. Store the smaller signed operand. AMO returns the old memory value in rd. RV64 only. |
| `amomax.d` | 64-bit operation. Store the larger signed operand. AMO returns the old memory value in rd. RV64 only. |
| `amominu.d` | 64-bit operation. Store the smaller unsigned operand. AMO returns the old memory value in rd. RV64 only. |
| `amomaxu.d` | 64-bit operation. Store the larger unsigned operand. AMO returns the old memory value in rd. RV64 only. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## C — compressed instructions

Compression changes encodings and operand restrictions, not the underlying arithmetic. Primed integer registers designate x8–x15. Stack-relative forms use x2. The same slot can mean C.JAL on RV32 or C.ADDIW on RV64, and C.FLW versus C.LD. Zca/Zcd/Zcf are not separate runtime switches in this source.

Implementation: [src/extensions/ext_c.cpp](../src/extensions/ext_c.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `c.addi4spn` | Expand to `addi rd′, sp, nonzero unsigned offset`; instruction length remains 2 bytes. |
| `c.lw` | Expand to `lw rd′, offset(rs1′)`; instruction length remains 2 bytes. |
| `c.sw` | Expand to `sw rs2′, offset(rs1′)`; instruction length remains 2 bytes. |
| `c.ld` | Expand to `ld rd′, offset(rs1′), RV64`; instruction length remains 2 bytes. |
| `c.sd` | Expand to `sd rs2′, offset(rs1′), RV64`; instruction length remains 2 bytes. |
| `c.fld` | Expand to `fld fd′, offset(rs1′), requires D`; instruction length remains 2 bytes. |
| `c.fsd` | Expand to `fsd fs2′, offset(rs1′), requires D`; instruction length remains 2 bytes. |
| `c.flw` | Expand to `flw fd′, offset(rs1′), RV32 with F`; instruction length remains 2 bytes. |
| `c.fsw` | Expand to `fsw fs2′, offset(rs1′), RV32 with F`; instruction length remains 2 bytes. |
| `c.nop` | Expand to `addi x0, x0, 0`; instruction length remains 2 bytes. |
| `c.addi` | Expand to `addi rd, rd, immediate`; instruction length remains 2 bytes. |
| `c.addiw` | Expand to `addiw rd, rd, immediate, RV64`; instruction length remains 2 bytes. |
| `c.jal` | Expand to `jal ra, offset, RV32`; instruction length remains 2 bytes. |
| `c.li` | Expand to `addi rd, x0, immediate`; instruction length remains 2 bytes. |
| `c.addi16sp` | Expand to `addi sp, sp, scaled nonzero immediate`; instruction length remains 2 bytes. |
| `c.lui` | Expand to `lui rd, immediate`; instruction length remains 2 bytes. |
| `c.srli` | Expand to `srli rd′, rd′, shift`; instruction length remains 2 bytes. |
| `c.srai` | Expand to `srai rd′, rd′, shift`; instruction length remains 2 bytes. |
| `c.andi` | Expand to `andi rd′, rd′, immediate`; instruction length remains 2 bytes. |
| `c.sub` | Expand to `sub rd′, rd′, rs2′`; instruction length remains 2 bytes. |
| `c.xor` | Expand to `xor rd′, rd′, rs2′`; instruction length remains 2 bytes. |
| `c.or` | Expand to `or rd′, rd′, rs2′`; instruction length remains 2 bytes. |
| `c.and` | Expand to `and rd′, rd′, rs2′`; instruction length remains 2 bytes. |
| `c.subw` | Expand to `subw rd′, rd′, rs2′, RV64`; instruction length remains 2 bytes. |
| `c.addw` | Expand to `addw rd′, rd′, rs2′, RV64`; instruction length remains 2 bytes. |
| `c.j` | Expand to `jal x0, offset`; instruction length remains 2 bytes. |
| `c.beqz` | Expand to `beq rs1′, x0, offset`; instruction length remains 2 bytes. |
| `c.bnez` | Expand to `bne rs1′, x0, offset`; instruction length remains 2 bytes. |
| `c.slli` | Expand to `slli rd, rd, shift`; instruction length remains 2 bytes. |
| `c.lwsp` | Expand to `lw rd, offset(sp)`; instruction length remains 2 bytes. |
| `c.ldsp` | Expand to `ld rd, offset(sp), RV64`; instruction length remains 2 bytes. |
| `c.fldsp` | Expand to `fld fd, offset(sp), D`; instruction length remains 2 bytes. |
| `c.flwsp` | Expand to `flw fd, offset(sp), RV32/F`; instruction length remains 2 bytes. |
| `c.jr` | Expand to `jalr x0, 0(rs1)`; instruction length remains 2 bytes. |
| `c.mv` | Expand to `add rd, x0, rs2`; instruction length remains 2 bytes. |
| `c.ebreak` | Expand to `ebreak`; instruction length remains 2 bytes. |
| `c.jalr` | Expand to `jalr ra, 0(rs1)`; instruction length remains 2 bytes. |
| `c.add` | Expand to `add rd, rd, rs2`; instruction length remains 2 bytes. |
| `c.swsp` | Expand to `sw rs2, offset(sp)`; instruction length remains 2 bytes. |
| `c.sdsp` | Expand to `sd rs2, offset(sp), RV64`; instruction length remains 2 bytes. |
| `c.fsdsp` | Expand to `fsd fs2, offset(sp), D`; instruction length remains 2 bytes. |
| `c.fswsp` | Expand to `fsw fs2, offset(sp), RV32/F`; instruction length remains 2 bytes. |

### CSR effects

No new CSRs. Floating compressed aliases use F/D state and flags; C.EBREAK changes trap state.

[Back to contents](#contents)

## Zcb — additional compressed operations

No Zcb flag exists. Memory aliases run under C; arithmetic aliases use M, Zba or Zbb where appropriate.

Implementation: [src/extensions/ext_zcb.cpp](../src/extensions/ext_zcb.cpp), [src/extensions/ext_c.cpp](../src/extensions/ext_c.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `c.lbu` | Load and zero-extend a byte. |
| `c.lhu` | Load and zero-extend a halfword. |
| `c.lh` | Load and sign-extend a halfword. |
| `c.sb` | Store a byte. |
| `c.sh` | Store a halfword. |
| `c.mul` | Multiply low integer bits through M. |
| `c.zext.b` | Clear all but low 8 bits using ANDI. |
| `c.sext.b` | Sign-extend low 8 bits through Zbb. |
| `c.zext.h` | Zero-extend low 16 bits through Zbb. |
| `c.sext.h` | Sign-extend low 16 bits through Zbb. |
| `c.zext.w` | RV64: zero-extend low 32 bits through Zba ADD.UW. |
| `c.not` | Invert bits using XORI -1. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## F — single precision floating point

32 FP registers use 64-bit storage. F represents binary32 with upper bits set for NaN boxing; D represents binary64 and requires F. Numerical operations use the shared helpers, with Berkeley SoftFloat used for core arithmetic and rounding-sensitive behavior; other conversions and vector paths still contain host arithmetic. This is not a claim that every FP path is independent of the host.

Implementation: [src/extensions/ext_f.cpp](../src/extensions/ext_f.cpp), [src/extensions/ext_fp_common.hpp](../src/extensions/ext_fp_common.hpp), [src/extensions/ext_softfloat.hpp](../src/extensions/ext_softfloat.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `fadd.s` | Add. |
| `fsub.s` | Subtract second operand from first. |
| `fmul.s` | Multiply. |
| `fdiv.s` | Divide first operand by second. |
| `fsqrt.s` | Square root. |
| `fmadd.s` | Fused a×b+c. |
| `fmsub.s` | Fused a×b−c. |
| `fnmsub.s` | Fused −a×b+c. |
| `fnmadd.s` | Fused −a×b−c. |
| `fsgnj.s` | Copy magnitude of a and sign of b. |
| `fsgnjn.s` | Copy magnitude of a and inverted sign of b. |
| `fsgnjx.s` | Copy magnitude of a and XOR the input sign bits. |
| `fmin.s` | Select minimum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `fmax.s` | Select maximum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `feq.s` | Integer result for equality; qNaN compares false without NV, sNaN raises NV. |
| `flt.s` | Integer result for less-than; NaNs compare false and raise NV. |
| `fle.s` | Integer result for less-or-equal; NaNs compare false and raise NV. |
| `fclass.s` | Return a 10-bit classification mask: negative infinity/normal/subnormal/zero, positive zero/subnormal/normal/infinity, signaling/quiet NaN. |
| `flw` | Load FP bits from translated memory; single values are NaN-boxed into 64-bit storage. |
| `fsw` | Store the low 32 raw bits, without arithmetic conversion. |
| `fcvt.w.s` | Convert floating value to signed 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.s.w` | Convert signed 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.wu.s` | Convert floating value to unsigned 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.s.wu` | Convert unsigned 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.l.s` | Convert floating value to signed 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.s.l` | Convert signed 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fcvt.lu.s` | Convert floating value to unsigned 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.s.lu` | Convert unsigned 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fmv.x.w` | Copy raw FP bits to integer register; W sign-extends bit 31, D requires RV64. |
| `fmv.w.x` | Copy raw integer bits to FP register; W is NaN-boxed, D requires RV64. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `fflags` | `0x001` | Accrued exception flags: NX bit 0 inexact, UF bit 1 underflow, OF bit 2 overflow, DZ bit 3 divide-by-zero, NV bit 4 invalid. Arithmetic ORs flags; CSR writes replace them. |
| `frm` | `0x002` | Dynamic rounding mode: 0 RNE, 1 RTZ, 2 RDN, 3 RUP, 4 RMM; instruction rm=7 requests this field. Reserved modes are not extra rounding algorithms. |
| `fcsr` | `0x003` | Combined view: frm in bits 7:5 and fflags in 4:0. Aliases the same dedicated storage. |

`mstatus.FS` tracks floating state (Off/Initial/Clean/Dirty); `mstatus.SD` summarizes dirty extension state. These are fields in existing privileged CSRs, not additional F CSRs. The current `sstatus` mask omits FS/VS/SD, despite historical comments suggesting a fuller alias.


[Back to contents](#contents)

## D — double precision floating point

32 FP registers use 64-bit storage. F represents binary32 with upper bits set for NaN boxing; D represents binary64 and requires F. Numerical operations use the shared helpers, with Berkeley SoftFloat used for core arithmetic and rounding-sensitive behavior; other conversions and vector paths still contain host arithmetic. This is not a claim that every FP path is independent of the host.

Implementation: [src/extensions/ext_d.cpp](../src/extensions/ext_d.cpp), [src/extensions/ext_fp_common.hpp](../src/extensions/ext_fp_common.hpp), [src/extensions/ext_softfloat.hpp](../src/extensions/ext_softfloat.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `fadd.d` | Add. |
| `fsub.d` | Subtract second operand from first. |
| `fmul.d` | Multiply. |
| `fdiv.d` | Divide first operand by second. |
| `fsqrt.d` | Square root. |
| `fmadd.d` | Fused a×b+c. |
| `fmsub.d` | Fused a×b−c. |
| `fnmsub.d` | Fused −a×b+c. |
| `fnmadd.d` | Fused −a×b−c. |
| `fsgnj.d` | Copy magnitude of a and sign of b. |
| `fsgnjn.d` | Copy magnitude of a and inverted sign of b. |
| `fsgnjx.d` | Copy magnitude of a and XOR the input sign bits. |
| `fmin.d` | Select minimum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `fmax.d` | Select maximum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `feq.d` | Integer result for equality; qNaN compares false without NV, sNaN raises NV. |
| `flt.d` | Integer result for less-than; NaNs compare false and raise NV. |
| `fle.d` | Integer result for less-or-equal; NaNs compare false and raise NV. |
| `fclass.d` | Return a 10-bit classification mask: negative infinity/normal/subnormal/zero, positive zero/subnormal/normal/infinity, signaling/quiet NaN. |
| `fld` | Load 64 raw bits into an FP register. |
| `fsd` | Store 64 raw bits from an FP register. |
| `fcvt.w.d` | Convert floating value to signed 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.d.w` | Convert signed 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.wu.d` | Convert floating value to unsigned 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.d.wu` | Convert unsigned 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.l.d` | Convert floating value to signed 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.d.l` | Convert signed 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fcvt.lu.d` | Convert floating value to unsigned 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.d.lu` | Convert unsigned 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fmv.x.d` | Copy raw FP bits to integer register; W sign-extends bit 31, D requires RV64. |
| `fmv.d.x` | Copy raw integer bits to FP register; W is NaN-boxed, D requires RV64. |
| `fcvt.s.d` | Round double to single; NaN handling and flags apply. |
| `fcvt.d.s` | Widen single to double; signaling NaN still raises invalid despite exact finite conversion. |

### CSR effects

No additional CSRs beyond F: `fflags`, `frm`, `fcsr` are shared. D also uses FS/SD state.

[Back to contents](#contents)

## Zfa — additional floating point

The implemented set is the S/D forms below. There are no half/quad Zfa operations or RV32 high-half/pair double moves in this decoder; the flag alone must not be interpreted as every Zfa format.

Implementation: [src/extensions/ext_zfa.cpp](../src/extensions/ext_zfa.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `fli.s` | Load one of 32 encoded constants; rs1 is an index, not a register value. |
| `fminm.s` | Minimum variant returning canonical NaN if either operand is NaN. |
| `fmaxm.s` | Maximum variant returning canonical NaN if either operand is NaN. |
| `fround.s` | Round to an integral value in the same FP format without reporting inexact for discarded fraction. |
| `froundnx.s` | Round to an integral FP value and report inexact. |
| `fleq.s` | Quiet less-or-equal: qNaN does not raise NV, sNaN does. |
| `fltq.s` | Quiet less-than with the same NaN flag rule. |
| `fli.d` | Load one of 32 encoded constants; rs1 is an index, not a register value. |
| `fminm.d` | Minimum variant returning canonical NaN if either operand is NaN. |
| `fmaxm.d` | Maximum variant returning canonical NaN if either operand is NaN. |
| `fround.d` | Round to an integral value in the same FP format without reporting inexact for discarded fraction. |
| `froundnx.d` | Round to an integral FP value and report inexact. |
| `fleq.d` | Quiet less-or-equal: qNaN does not raise NV, sNaN does. |
| `fltq.d` | Quiet less-than with the same NaN flag rule. |
| `fcvtmod.w.d` | Truncate double toward zero and wrap modulo 2^32, then sign-extend the word result; unlike saturating float-to-integer conversions. |

### CSR effects

Reuses F `fflags`, `frm`, `fcsr`; adds no CSR.

[Back to contents](#contents)

## Zfhmin — minimal scalar half precision

Storage, raw moves and conversions for IEEE binary16. Full Zfh arithmetic such as FADD.H is not implemented. Half conversions use integer significand handling to avoid double rounding.

Implementation: [src/extensions/ext_zfhmin.cpp](../src/extensions/ext_zfhmin.cpp), [src/extensions/ext_fp16.hpp](../src/extensions/ext_fp16.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `flh` | Load 16 raw bits and NaN-box into the FP register. |
| `fsh` | Store low 16 raw bits; do not reinterpret bad NaN boxing as canonical NaN for this raw store. |
| `fmv.x.h` | Copy low 16 raw bits to x register and sign-extend. |
| `fmv.h.x` | Copy low 16 x-register bits into a NaN-boxed FP register. |
| `fcvt.s.h` | Widen binary16 to binary32; finite values are exact. |
| `fcvt.h.s` | Round binary32 to binary16 and accrue conversion flags. |
| `fcvt.d.h` | Widen binary16 to binary64, involving D. |
| `fcvt.h.d` | Round binary64 directly to binary16, avoiding a binary32 intermediate. |

### CSR effects

Shares F CSRs; no new CSR.

[Back to contents](#contents)

## Zba — address generation

Scaled indexing and unsigned-word address arithmetic.

Implementation: [src/extensions/ext_zba.cpp](../src/extensions/ext_zba.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `sh1add` | Compute (rs1 << 1) + rs2 at XLEN. |
| `sh2add` | Compute (rs1 << 2) + rs2 at XLEN. |
| `sh3add` | Compute (rs1 << 3) + rs2 at XLEN. |
| `add.uw` | RV64: zero-extend rs1[31:0] and add full rs2. |
| `sh1add.uw` | RV64: zero-extend rs1[31:0], shift left 1, then add full rs2. |
| `sh2add.uw` | RV64: zero-extend rs1[31:0], shift left 2, then add full rs2. |
| `sh3add.uw` | RV64: zero-extend rs1[31:0], shift left 3, then add full rs2. |
| `slli.uw` | RV64: zero-extend the low word before a full-width immediate left shift. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zbb — basic bit manipulation

Boolean, counting, rotate, min/max and extension operations. The current helper implementation contains 64-bit-specific paths; selecting RV32 should not be taken as proof of complete RV32 Zbb behavior.

Implementation: [src/extensions/ext_zbb.cpp](../src/extensions/ext_zbb.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `andn` | rs1 AND complement(rs2). |
| `orn` | rs1 OR complement(rs2). |
| `xnor` | Complement of rs1 XOR rs2. |
| `clz` | Count leading zero bits; all-zero input returns the operation width. |
| `ctz` | Count trailing zero bits. |
| `cpop` | Count set bits. |
| `clzw` | RV64: count leading zero bits in low 32 bits. |
| `ctzw` | RV64: count trailing zero bits in low 32 bits. |
| `cpopw` | RV64: population count of low 32 bits. |
| `min` | Select smaller signed operand. |
| `minu` | Select smaller unsigned operand. |
| `max` | Select larger signed operand. |
| `maxu` | Select larger unsigned operand. |
| `sext.b` | Sign-extend low byte. |
| `sext.h` | Sign-extend low halfword. |
| `zext.h` | Zero-extend low halfword. |
| `rol` | Rotate left by register count. |
| `ror` | Rotate right by register count. |
| `rori` | Rotate right by immediate count. |
| `rolw` | RV64: rotate low 32 bits left and sign-extend. |
| `rorw` | RV64: rotate low 32 bits right and sign-extend. |
| `roriw` | RV64: immediate rotate of low 32 bits, sign-extended. |
| `orc.b` | Replace each nonzero byte by 0xff; preserve zero bytes. |
| `rev8` | Reverse the order of bytes across the register. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zbs — single-bit manipulation

Bit indices are operation-width positions, not memory addresses.

Implementation: [src/extensions/ext_zbs.cpp](../src/extensions/ext_zbs.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `bset` | Set the selected bit to 1; index comes from rs2. |
| `bseti` | Set the selected bit to 1; index is immediate. |
| `bclr` | Clear the selected bit; index comes from rs2. |
| `bclri` | Clear the selected bit; index is immediate. |
| `binv` | Invert the selected bit; index comes from rs2. |
| `binvi` | Invert the selected bit; index is immediate. |
| `bext` | Return the selected bit in rd[0], clearing the other bits; index comes from rs2. |
| `bexti` | Return the selected bit in rd[0], clearing the other bits; index is immediate. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zicond — conditional zero

Build conditional selection without a branch.

Implementation: [src/extensions/ext_zicond.cpp](../src/extensions/ext_zicond.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `czero.eqz` | rd=0 when rs2==0, otherwise rd=rs1. |
| `czero.nez` | rd=0 when rs2!=0, otherwise rd=rs1. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zicsr — CSR access instructions

CSR numbers encode minimum privilege in bits 9:8 and read-only status in bits 11:10. A CSR is not an MMIO address. The executor handles ordinary storage, computed aliases, WARL writes and device-backed side effects. Unknown CSR numbers still fall back to generic storage; successful access does not prove an extension exists.

Implementation: [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `csrrw` | Read old CSR into rd and replace it with rs1. Architecturally rd=x0 suppresses the read; DoomV still computes an old value, though its implemented effective reads are side-effect-free. |
| `csrrs` | Read CSR; OR in rs1. rs1=x0 suppresses the write, but a nonzero register number holding zero still requests a write. |
| `csrrc` | Read CSR; clear bits set in rs1. rs1=x0 suppresses the write. |
| `csrrwi` | Replace CSR with zero-extended 5-bit immediate and return old value. |
| `csrrsi` | Set bits selected by 5-bit immediate; immediate zero is a pure read. |
| `csrrci` | Clear bits selected by 5-bit immediate; immediate zero is a pure read. |

### CSR effects

Zicsr defines the access instructions, **not the existence of every CSR**. Each owning extension below lists its state. `csrr`, `csrw`, `csrs` and `csrc` are assembler aliases. Reading `mtopei/stopei` is a peek; actual writes claim an interrupt.

[Back to contents](#contents)

## Zifencei — instruction-fetch synchronization

The decoder cache is not an architectural instruction cache: it rechecks live instruction bytes before using a decode. Thus modified code misses the old tag.

Implementation: [src/extensions/ext_zifencei.cpp](../src/extensions/ext_zifencei.cpp), [src/riscv_decoder.cpp](../src/riscv_decoder.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `fence.i` | Make previous stores visible to later instruction fetches on this hart; no extra flush is required by this implementation. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zihintpause — pause hint

Hints express performance intent without requiring blocking.

Implementation: [src/extensions/ext_zihintpause.cpp](../src/extensions/ext_zihintpause.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `pause` | Retire immediately. With its flag off, the encoding can retain its legal base FENCE behavior. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zihintntl — non-temporal locality hints

No cache placement policy is modeled. The explicit hint helper handles compressed forms; normal ADD-to-x0 semantics cover the wide forms.

Implementation: [src/extensions/ext_zihintntl.cpp](../src/extensions/ext_zihintntl.cpp), [src/extensions/ext_c.cpp](../src/extensions/ext_c.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `ntl.p1` | No temporal locality within the innermost private cache; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.p1` | No temporal locality within the innermost private cache; compressed hint has an explicit decoder name. |
| `ntl.pall` | No temporal locality in private caches; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.pall` | No temporal locality in private caches; compressed hint has an explicit decoder name. |
| `ntl.s1` | No temporal locality through the innermost shared cache; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.s1` | No temporal locality through the innermost shared cache; compressed hint has an explicit decoder name. |
| `ntl.all` | No temporal locality through all cache levels; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.all` | No temporal locality through all cache levels; compressed hint has an explicit decoder name. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zimop — may-be operations

Reserved expansion space with defined fallback semantics. Numeric members are separately listed because future extensions may assign meaning to individual encodings.

Implementation: [src/extensions/ext_zimop.cpp](../src/extensions/ext_zimop.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `mop.r.0` | One source operand; current fallback writes zero to rd. |
| `mop.r.1` | One source operand; current fallback writes zero to rd. |
| `mop.r.2` | One source operand; current fallback writes zero to rd. |
| `mop.r.3` | One source operand; current fallback writes zero to rd. |
| `mop.r.4` | One source operand; current fallback writes zero to rd. |
| `mop.r.5` | One source operand; current fallback writes zero to rd. |
| `mop.r.6` | One source operand; current fallback writes zero to rd. |
| `mop.r.7` | One source operand; current fallback writes zero to rd. |
| `mop.r.8` | One source operand; current fallback writes zero to rd. |
| `mop.r.9` | One source operand; current fallback writes zero to rd. |
| `mop.r.10` | One source operand; current fallback writes zero to rd. |
| `mop.r.11` | One source operand; current fallback writes zero to rd. |
| `mop.r.12` | One source operand; current fallback writes zero to rd. |
| `mop.r.13` | One source operand; current fallback writes zero to rd. |
| `mop.r.14` | One source operand; current fallback writes zero to rd. |
| `mop.r.15` | One source operand; current fallback writes zero to rd. |
| `mop.r.16` | One source operand; current fallback writes zero to rd. |
| `mop.r.17` | One source operand; current fallback writes zero to rd. |
| `mop.r.18` | One source operand; current fallback writes zero to rd. |
| `mop.r.19` | One source operand; current fallback writes zero to rd. |
| `mop.r.20` | One source operand; current fallback writes zero to rd. |
| `mop.r.21` | One source operand; current fallback writes zero to rd. |
| `mop.r.22` | One source operand; current fallback writes zero to rd. |
| `mop.r.23` | One source operand; current fallback writes zero to rd. |
| `mop.r.24` | One source operand; current fallback writes zero to rd. |
| `mop.r.25` | One source operand; current fallback writes zero to rd. |
| `mop.r.26` | One source operand; current fallback writes zero to rd. |
| `mop.r.27` | One source operand; current fallback writes zero to rd. |
| `mop.r.28` | One source operand; current fallback writes zero to rd. |
| `mop.r.29` | One source operand; current fallback writes zero to rd. |
| `mop.r.30` | One source operand; current fallback writes zero to rd. |
| `mop.r.31` | One source operand; current fallback writes zero to rd. |
| `mop.rr.0` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.1` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.2` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.3` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.4` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.5` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.6` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.7` | Two source operands; current fallback writes zero to rd. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zcmop — compressed may-be operations

Compressed expansion space. These encodings have no destination register and leave all registers unchanged; the helper only advances PC. Parser enabling Zcmop also enables Zimop.

Implementation: [src/extensions/ext_zcmop.cpp](../src/extensions/ext_zcmop.cpp), [src/extensions/ext_c.cpp](../src/extensions/ext_c.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `c.mop.1` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.3` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.5` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.7` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.9` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.11` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.13` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.15` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zicbom — cache-block management

Block size is 64 bytes. There is no data cache, but these instructions translate an aligned-down address and check read OR write permission, without requiring the PTE dirty bit. Failure uses store-class fault reporting. Environment CBIE/CBCFE controls are not enforced in the current helper; checking page/PMP access is a different layer.

Implementation: [src/extensions/ext_zicbom.cpp](../src/extensions/ext_zicbom.cpp), [src/mmu.cpp](../src/mmu.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `cbo.inval` | Invalidate cached copies architecturally; current memory contents remain unchanged after access checks. |
| `cbo.clean` | Write back dirty cached data architecturally; no cache writeback exists here. |
| `cbo.flush` | Clean and invalidate architecturally; no cached state exists here. |

### CSR effects

No new CSR. Architectural control fields occupy `menvcfg.CBIE[5:4]`, `CBCFE[6]` and corresponding lower/virtual environment controls; these are not fully modeled permissions in this snapshot.

[Back to contents](#contents)

## Zicbop — prefetch hints

Encoded in ORI-to-x0 hint space, so disabling the label need not make the encoding illegal.

Implementation: [src/extensions/ext_zicbop.cpp](../src/extensions/ext_zicbop.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `prefetch.i` | Hint that instruction fetch will use the addressed block; no memory access or prefetch state is generated. |
| `prefetch.r` | Hint that data will be read; retires without effect. |
| `prefetch.w` | Hint that data will be written; retires without effect. |

### CSR effects

No new CSRs.

[Back to contents](#contents)

## Zicboz — block zero

Unlike cache-maintenance hints, zeroing changes guest-visible bytes.

Implementation: [src/extensions/ext_zicboz.cpp](../src/extensions/ext_zicboz.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `cbo.zero` | Align rs1 down to 64 bytes, then write eight zero doublewords through store translation. Earlier chunks remain zero if a later access faults. The helper currently omits an explicit 8-byte size argument to translate_or_trap. |

### CSR effects

No new CSR; architectural `menvcfg.CBZE[7]` and lower/virtual counterparts control permission, but this executor does not enforce that environment gate.

[Back to contents](#contents)

## Zawrs — wait on reservation set

Both encodings use SYSTEM space and run through a dedicated helper. Immediate completion prevents a single-hart interpreter from waiting indefinitely for another hart.

Implementation: [src/extensions/ext_zawrs.cpp](../src/extensions/ext_zawrs.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `wrs.nto` | Reservation-set wait without a specified timeout; DoomV returns immediately. |
| `wrs.sto` | Reservation-set wait with short timeout; DoomV returns immediately. |

### CSR effects

No CSR. Reservation state is internal; privilege/timeout controls are not comprehensively modeled.

[Back to contents](#contents)

## Zicntr — basic counters

These are CSR reads, not new opcodes. RDCYCLE/RDTIME/RDINSTRET are CSRRS aliases. All three effective values read Timer::mtime. The name instret is misleading as an exact retirement statistic here: DoomSystem also calls step_instructions on interrupt-entry and fault paths.

Implementation: [src/extensions/ext_zicntr.cpp](../src/extensions/ext_zicntr.cpp), [src/extensions/ext_zicntr.hpp](../src/extensions/ext_zicntr.hpp), [src/doom_system.cpp](../src/doom_system.cpp).

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

Implementation: [src/extensions/ext_zicntr.cpp](../src/extensions/ext_zicntr.cpp).

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

## V — vector configuration and execution model

V is opt-in. `VLEN=128`, supported ordinary element widths are 8/16/32/64 and register groups implement LMUL. For arithmetic, .vv supplies vector operands, .vx an integer scalar, .vi an immediate, .vf an FP scalar. These suffixes are **not** universally interchangeable. The following tables enumerate distinct mnemonic forms; memory families expand their encoded widths and field counts.

Execution comes from funct fields, not the dashboard mnemonic: some display names are stale or generic. Enabled V does not guarantee legality checking for every reserved encoding, overlap, SEW/EMUL combination or restart condition. Several handlers return silently for unsupported cases. Treat the listed operation meaning as the intended architectural operation and consult the limitation notes for the source behavior.

Implementation: [src/extensions/ext_v.cpp](../src/extensions/ext_v.cpp), [src/extensions/ext_v_config.cpp](../src/extensions/ext_v_config.cpp), [src/extensions/ext_v_common.hpp](../src/extensions/ext_v_common.hpp), [src/registers.hpp](../src/registers.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `vsetvli` | AVL comes from rs1 and vtype from immediate; rd receives selected vl. |
| `vsetivli` | AVL is a 5-bit immediate and vtype is immediate. |
| `vsetvl` | AVL and vtype come from integer registers; useful for restoring saved state. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `vstart` | `0x008` | Index of the element at which execution starts/restarts. Normal completion resets it; ordinary vector memory faults record the failing element. |
| `vxsat` | `0x009` | Sticky fixed-point saturation flag. |
| `vxrm` | `0x00A` | Fixed-point rounding mode: rnu, rne, rdn, rod. |
| `vcsr` | `0x00F` | Combined vxrm[2:1] and vxsat[0] view. |
| `vl` | `0xC20` | Current active element count; written by vset instructions and fault-only-first shortening. |
| `vtype` | `0xC21` | LMUL[2:0], VSEW[5:3], VTA[6], VMA[7], VILL[XLEN-1]. Invalid configuration yields vl=0. |
| `vlenb` | `0xC22` | Read-only 16 bytes: VLEN=128 bits. |

Uses `mstatus.VS` for state enable/dirty tracking. FP vector operations share F flags and rounding. Storage is 32 arrays of 16 bytes; grouping and effective element width are implemented by shared byte-addressing helpers.


[Back to contents](#contents)

## V — vector memory instructions

The common memory loop handles unit, strided, indexed and segmented forms. Segment address is base + element stride + field offset. Indexed operations use a vector of byte offsets; ordered versus unordered variants currently use the same sequential host loop. Masked-off elements skip access.

Fault-only-first element 0 faults normally; a later translation failure shortens vl and completes. The later-element probe calls mmu_translate directly, bypassing the final physical-access wrapper. Ordinary vector accesses also omit a full element size in some wrapper calls. Whole-register/mask paths have distinct loops and must not be assumed to share every ordinary-element restart behavior.

Implementation: [src/extensions/ext_v_ldst.cpp](../src/extensions/ext_v_ldst.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `vle8.v` | Unit-stride load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse8.v` | Unit-stride store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse8.v` | Signed byte-stride load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse8.v` | Signed byte-stride store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei8.v` | Unordered indexed load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei8.v` | Ordered indexed load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei8.v` | Unordered indexed store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei8.v` | Ordered indexed store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle8ff.v` | Fault-only-first unit-stride load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e8.v` | Unit-stride segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e8.v` | Unit-stride segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e8.v` | Strided segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e8.v` | Strided segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei8.v` | Unordered indexed segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei8.v` | Ordered indexed segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei8.v` | Unordered indexed segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei8.v` | Ordered indexed segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e8ff.v` | Fault-only-first segmented load of 2 fields of 8-bit elements. |
| `vlseg3e8.v` | Unit-stride segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e8.v` | Unit-stride segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e8.v` | Strided segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e8.v` | Strided segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei8.v` | Unordered indexed segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei8.v` | Ordered indexed segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei8.v` | Unordered indexed segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei8.v` | Ordered indexed segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e8ff.v` | Fault-only-first segmented load of 3 fields of 8-bit elements. |
| `vlseg4e8.v` | Unit-stride segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e8.v` | Unit-stride segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e8.v` | Strided segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e8.v` | Strided segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei8.v` | Unordered indexed segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei8.v` | Ordered indexed segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei8.v` | Unordered indexed segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei8.v` | Ordered indexed segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e8ff.v` | Fault-only-first segmented load of 4 fields of 8-bit elements. |
| `vlseg5e8.v` | Unit-stride segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e8.v` | Unit-stride segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e8.v` | Strided segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e8.v` | Strided segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei8.v` | Unordered indexed segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei8.v` | Ordered indexed segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei8.v` | Unordered indexed segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei8.v` | Ordered indexed segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e8ff.v` | Fault-only-first segmented load of 5 fields of 8-bit elements. |
| `vlseg6e8.v` | Unit-stride segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e8.v` | Unit-stride segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e8.v` | Strided segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e8.v` | Strided segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei8.v` | Unordered indexed segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei8.v` | Ordered indexed segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei8.v` | Unordered indexed segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei8.v` | Ordered indexed segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e8ff.v` | Fault-only-first segmented load of 6 fields of 8-bit elements. |
| `vlseg7e8.v` | Unit-stride segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e8.v` | Unit-stride segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e8.v` | Strided segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e8.v` | Strided segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei8.v` | Unordered indexed segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei8.v` | Ordered indexed segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei8.v` | Unordered indexed segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei8.v` | Ordered indexed segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e8ff.v` | Fault-only-first segmented load of 7 fields of 8-bit elements. |
| `vlseg8e8.v` | Unit-stride segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e8.v` | Unit-stride segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e8.v` | Strided segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e8.v` | Strided segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei8.v` | Unordered indexed segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei8.v` | Ordered indexed segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei8.v` | Unordered indexed segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei8.v` | Ordered indexed segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e8ff.v` | Fault-only-first segmented load of 8 fields of 8-bit elements. |
| `vle16.v` | Unit-stride load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse16.v` | Unit-stride store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse16.v` | Signed byte-stride load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse16.v` | Signed byte-stride store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei16.v` | Unordered indexed load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei16.v` | Ordered indexed load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei16.v` | Unordered indexed store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei16.v` | Ordered indexed store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle16ff.v` | Fault-only-first unit-stride load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e16.v` | Unit-stride segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e16.v` | Unit-stride segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e16.v` | Strided segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e16.v` | Strided segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei16.v` | Unordered indexed segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei16.v` | Ordered indexed segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei16.v` | Unordered indexed segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei16.v` | Ordered indexed segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e16ff.v` | Fault-only-first segmented load of 2 fields of 16-bit elements. |
| `vlseg3e16.v` | Unit-stride segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e16.v` | Unit-stride segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e16.v` | Strided segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e16.v` | Strided segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei16.v` | Unordered indexed segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei16.v` | Ordered indexed segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei16.v` | Unordered indexed segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei16.v` | Ordered indexed segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e16ff.v` | Fault-only-first segmented load of 3 fields of 16-bit elements. |
| `vlseg4e16.v` | Unit-stride segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e16.v` | Unit-stride segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e16.v` | Strided segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e16.v` | Strided segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei16.v` | Unordered indexed segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei16.v` | Ordered indexed segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei16.v` | Unordered indexed segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei16.v` | Ordered indexed segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e16ff.v` | Fault-only-first segmented load of 4 fields of 16-bit elements. |
| `vlseg5e16.v` | Unit-stride segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e16.v` | Unit-stride segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e16.v` | Strided segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e16.v` | Strided segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei16.v` | Unordered indexed segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei16.v` | Ordered indexed segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei16.v` | Unordered indexed segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei16.v` | Ordered indexed segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e16ff.v` | Fault-only-first segmented load of 5 fields of 16-bit elements. |
| `vlseg6e16.v` | Unit-stride segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e16.v` | Unit-stride segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e16.v` | Strided segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e16.v` | Strided segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei16.v` | Unordered indexed segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei16.v` | Ordered indexed segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei16.v` | Unordered indexed segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei16.v` | Ordered indexed segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e16ff.v` | Fault-only-first segmented load of 6 fields of 16-bit elements. |
| `vlseg7e16.v` | Unit-stride segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e16.v` | Unit-stride segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e16.v` | Strided segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e16.v` | Strided segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei16.v` | Unordered indexed segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei16.v` | Ordered indexed segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei16.v` | Unordered indexed segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei16.v` | Ordered indexed segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e16ff.v` | Fault-only-first segmented load of 7 fields of 16-bit elements. |
| `vlseg8e16.v` | Unit-stride segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e16.v` | Unit-stride segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e16.v` | Strided segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e16.v` | Strided segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei16.v` | Unordered indexed segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei16.v` | Ordered indexed segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei16.v` | Unordered indexed segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei16.v` | Ordered indexed segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e16ff.v` | Fault-only-first segmented load of 8 fields of 16-bit elements. |
| `vle32.v` | Unit-stride load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse32.v` | Unit-stride store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse32.v` | Signed byte-stride load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse32.v` | Signed byte-stride store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei32.v` | Unordered indexed load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei32.v` | Ordered indexed load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei32.v` | Unordered indexed store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei32.v` | Ordered indexed store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle32ff.v` | Fault-only-first unit-stride load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e32.v` | Unit-stride segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e32.v` | Unit-stride segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e32.v` | Strided segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e32.v` | Strided segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei32.v` | Unordered indexed segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei32.v` | Ordered indexed segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei32.v` | Unordered indexed segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei32.v` | Ordered indexed segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e32ff.v` | Fault-only-first segmented load of 2 fields of 32-bit elements. |
| `vlseg3e32.v` | Unit-stride segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e32.v` | Unit-stride segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e32.v` | Strided segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e32.v` | Strided segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei32.v` | Unordered indexed segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei32.v` | Ordered indexed segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei32.v` | Unordered indexed segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei32.v` | Ordered indexed segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e32ff.v` | Fault-only-first segmented load of 3 fields of 32-bit elements. |
| `vlseg4e32.v` | Unit-stride segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e32.v` | Unit-stride segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e32.v` | Strided segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e32.v` | Strided segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei32.v` | Unordered indexed segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei32.v` | Ordered indexed segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei32.v` | Unordered indexed segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei32.v` | Ordered indexed segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e32ff.v` | Fault-only-first segmented load of 4 fields of 32-bit elements. |
| `vlseg5e32.v` | Unit-stride segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e32.v` | Unit-stride segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e32.v` | Strided segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e32.v` | Strided segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei32.v` | Unordered indexed segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei32.v` | Ordered indexed segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei32.v` | Unordered indexed segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei32.v` | Ordered indexed segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e32ff.v` | Fault-only-first segmented load of 5 fields of 32-bit elements. |
| `vlseg6e32.v` | Unit-stride segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e32.v` | Unit-stride segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e32.v` | Strided segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e32.v` | Strided segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei32.v` | Unordered indexed segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei32.v` | Ordered indexed segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei32.v` | Unordered indexed segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei32.v` | Ordered indexed segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e32ff.v` | Fault-only-first segmented load of 6 fields of 32-bit elements. |
| `vlseg7e32.v` | Unit-stride segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e32.v` | Unit-stride segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e32.v` | Strided segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e32.v` | Strided segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei32.v` | Unordered indexed segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei32.v` | Ordered indexed segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei32.v` | Unordered indexed segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei32.v` | Ordered indexed segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e32ff.v` | Fault-only-first segmented load of 7 fields of 32-bit elements. |
| `vlseg8e32.v` | Unit-stride segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e32.v` | Unit-stride segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e32.v` | Strided segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e32.v` | Strided segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei32.v` | Unordered indexed segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei32.v` | Ordered indexed segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei32.v` | Unordered indexed segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei32.v` | Ordered indexed segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e32ff.v` | Fault-only-first segmented load of 8 fields of 32-bit elements. |
| `vle64.v` | Unit-stride load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse64.v` | Unit-stride store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse64.v` | Signed byte-stride load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse64.v` | Signed byte-stride store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei64.v` | Unordered indexed load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei64.v` | Ordered indexed load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei64.v` | Unordered indexed store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei64.v` | Ordered indexed store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle64ff.v` | Fault-only-first unit-stride load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e64.v` | Unit-stride segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e64.v` | Unit-stride segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e64.v` | Strided segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e64.v` | Strided segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei64.v` | Unordered indexed segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei64.v` | Ordered indexed segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei64.v` | Unordered indexed segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei64.v` | Ordered indexed segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e64ff.v` | Fault-only-first segmented load of 2 fields of 64-bit elements. |
| `vlseg3e64.v` | Unit-stride segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e64.v` | Unit-stride segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e64.v` | Strided segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e64.v` | Strided segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei64.v` | Unordered indexed segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei64.v` | Ordered indexed segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei64.v` | Unordered indexed segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei64.v` | Ordered indexed segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e64ff.v` | Fault-only-first segmented load of 3 fields of 64-bit elements. |
| `vlseg4e64.v` | Unit-stride segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e64.v` | Unit-stride segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e64.v` | Strided segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e64.v` | Strided segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei64.v` | Unordered indexed segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei64.v` | Ordered indexed segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei64.v` | Unordered indexed segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei64.v` | Ordered indexed segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e64ff.v` | Fault-only-first segmented load of 4 fields of 64-bit elements. |
| `vlseg5e64.v` | Unit-stride segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e64.v` | Unit-stride segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e64.v` | Strided segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e64.v` | Strided segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei64.v` | Unordered indexed segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei64.v` | Ordered indexed segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei64.v` | Unordered indexed segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei64.v` | Ordered indexed segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e64ff.v` | Fault-only-first segmented load of 5 fields of 64-bit elements. |
| `vlseg6e64.v` | Unit-stride segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e64.v` | Unit-stride segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e64.v` | Strided segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e64.v` | Strided segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei64.v` | Unordered indexed segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei64.v` | Ordered indexed segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei64.v` | Unordered indexed segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei64.v` | Ordered indexed segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e64ff.v` | Fault-only-first segmented load of 6 fields of 64-bit elements. |
| `vlseg7e64.v` | Unit-stride segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e64.v` | Unit-stride segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e64.v` | Strided segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e64.v` | Strided segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei64.v` | Unordered indexed segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei64.v` | Ordered indexed segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei64.v` | Unordered indexed segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei64.v` | Ordered indexed segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e64ff.v` | Fault-only-first segmented load of 7 fields of 64-bit elements. |
| `vlseg8e64.v` | Unit-stride segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e64.v` | Unit-stride segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e64.v` | Strided segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e64.v` | Strided segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei64.v` | Unordered indexed segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei64.v` | Ordered indexed segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei64.v` | Unordered indexed segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei64.v` | Ordered indexed segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e64ff.v` | Fault-only-first segmented load of 8 fields of 64-bit elements. |
| `vl1re8.v` | Load 1 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl1re16.v` | Load 1 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl1re32.v` | Load 1 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl1re64.v` | Load 1 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs1r.v` | Store 1 whole register(s), independent of current vl. |
| `vl2re8.v` | Load 2 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl2re16.v` | Load 2 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl2re32.v` | Load 2 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl2re64.v` | Load 2 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs2r.v` | Store 2 whole register(s), independent of current vl. |
| `vl4re8.v` | Load 4 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl4re16.v` | Load 4 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl4re32.v` | Load 4 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl4re64.v` | Load 4 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs4r.v` | Store 4 whole register(s), independent of current vl. |
| `vl8re8.v` | Load 8 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl8re16.v` | Load 8 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl8re32.v` | Load 8 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl8re64.v` | Load 8 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs8r.v` | Store 8 whole register(s), independent of current vl. |
| `vlm.v` | Load ceil(vl/8) packed mask bytes. |
| `vsm.v` | Store ceil(vl/8) packed mask bytes. |

### CSR effects

Uses V CSRs; fault-only-first may change vl, ordinary element faults may change vstart. No memory-family-specific CSR.

[Back to contents](#contents)

## V — integer and fixed-point instructions

Arithmetic runs element by element through shared iteration helpers. Tail/mask-undisturbed behavior leaves old bytes, an allowed choice for many agnostic results; it does not establish comprehensive agnostic-policy validation. Widened integer paths are limited by the helper widths, particularly at SEW=64.

Implementation: [src/extensions/ext_v_int.cpp](../src/extensions/ext_v_int.cpp), [src/extensions/ext_v_muldiv.cpp](../src/extensions/ext_v_muldiv.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `vadd.vv` | Elementwise addition modulo SEW. |
| `vadd.vx` | Elementwise addition modulo SEW. |
| `vadd.vi` | Elementwise addition modulo SEW. |
| `vsub.vv` | vs2 minus second operand. |
| `vsub.vx` | vs2 minus second operand. |
| `vrsub.vx` | Scalar/immediate minus vs2. |
| `vrsub.vi` | Scalar/immediate minus vs2. |
| `vminu.vv` | Unsigned minimum. |
| `vminu.vx` | Unsigned minimum. |
| `vmin.vv` | Signed minimum. |
| `vmin.vx` | Signed minimum. |
| `vmaxu.vv` | Unsigned maximum. |
| `vmaxu.vx` | Unsigned maximum. |
| `vmax.vv` | Signed maximum. |
| `vmax.vx` | Signed maximum. |
| `vand.vv` | Bitwise AND. |
| `vand.vx` | Bitwise AND. |
| `vand.vi` | Bitwise AND. |
| `vor.vv` | Bitwise OR. |
| `vor.vx` | Bitwise OR. |
| `vor.vi` | Bitwise OR. |
| `vxor.vv` | Bitwise XOR. |
| `vxor.vx` | Bitwise XOR. |
| `vxor.vi` | Bitwise XOR. |
| `vadc.vvm` | Add with carry-in from v0; v0 supplies data, not an execution mask. |
| `vadc.vxm` | Add with carry-in from v0; v0 supplies data, not an execution mask. |
| `vadc.vim` | Add with carry-in from v0; v0 supplies data, not an execution mask. |
| `vmadc.vvm` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vxm` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vim` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vv` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vx` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vi` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vsbc.vvm` | Subtract with v0 borrow-in. |
| `vsbc.vxm` | Subtract with v0 borrow-in. |
| `vmsbc.vvm` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmsbc.vxm` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmsbc.vv` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmsbc.vx` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmerge.vvm` | Select source per v0 bit. |
| `vmerge.vxm` | Select source per v0 bit. |
| `vmerge.vim` | Select source per v0 bit. |
| `vmv.v.v` | Copy/broadcast source to active destination elements. |
| `vmv.v.x` | Copy/broadcast source to active destination elements. |
| `vmv.v.i` | Copy/broadcast source to active destination elements. |
| `vmseq.vv` | Produce equality mask. |
| `vmseq.vx` | Produce equality mask. |
| `vmseq.vi` | Produce equality mask. |
| `vmsne.vv` | Produce inequality mask. |
| `vmsne.vx` | Produce inequality mask. |
| `vmsne.vi` | Produce inequality mask. |
| `vmsltu.vv` | Produce unsigned less-than mask. |
| `vmsltu.vx` | Produce unsigned less-than mask. |
| `vmslt.vv` | Produce signed less-than mask. |
| `vmslt.vx` | Produce signed less-than mask. |
| `vmsleu.vv` | Produce unsigned less-or-equal mask. |
| `vmsleu.vx` | Produce unsigned less-or-equal mask. |
| `vmsleu.vi` | Produce unsigned less-or-equal mask. |
| `vmsle.vv` | Produce signed less-or-equal mask. |
| `vmsle.vx` | Produce signed less-or-equal mask. |
| `vmsle.vi` | Produce signed less-or-equal mask. |
| `vmsgtu.vx` | Produce unsigned greater-than mask. |
| `vmsgtu.vi` | Produce unsigned greater-than mask. |
| `vmsgt.vx` | Produce signed greater-than mask. |
| `vmsgt.vi` | Produce signed greater-than mask. |
| `vsll.vv` | Logical left shift, count masked to element width. |
| `vsll.vx` | Logical left shift, count masked to element width. |
| `vsll.vi` | Logical left shift, count masked to element width. |
| `vsrl.vv` | Logical right shift. |
| `vsrl.vx` | Logical right shift. |
| `vsrl.vi` | Logical right shift. |
| `vsra.vv` | Arithmetic right shift. |
| `vsra.vx` | Arithmetic right shift. |
| `vsra.vi` | Arithmetic right shift. |
| `vsaddu.vv` | Saturating unsigned add, sets vxsat when clamped. |
| `vsaddu.vx` | Saturating unsigned add, sets vxsat when clamped. |
| `vsaddu.vi` | Saturating unsigned add, sets vxsat when clamped. |
| `vsadd.vv` | Saturating signed add, sets vxsat when clamped. |
| `vsadd.vx` | Saturating signed add, sets vxsat when clamped. |
| `vsadd.vi` | Saturating signed add, sets vxsat when clamped. |
| `vssubu.vv` | Saturating unsigned subtract. |
| `vssubu.vx` | Saturating unsigned subtract. |
| `vssub.vv` | Saturating signed subtract. |
| `vssub.vx` | Saturating signed subtract. |
| `vaaddu.vv` | Unsigned averaging add using vxrm. |
| `vaaddu.vx` | Unsigned averaging add using vxrm. |
| `vaadd.vv` | Signed averaging add using vxrm. |
| `vaadd.vx` | Signed averaging add using vxrm. |
| `vasubu.vv` | Unsigned averaging subtract using vxrm. |
| `vasubu.vx` | Unsigned averaging subtract using vxrm. |
| `vasub.vv` | Signed averaging subtract using vxrm. |
| `vasub.vx` | Signed averaging subtract using vxrm. |
| `vsmul.vv` | Signed fractional multiply with rounding and saturation. |
| `vsmul.vx` | Signed fractional multiply with rounding and saturation. |
| `vssrl.vv` | Rounded logical scaling right shift using vxrm. |
| `vssrl.vx` | Rounded logical scaling right shift using vxrm. |
| `vssrl.vi` | Rounded logical scaling right shift using vxrm. |
| `vssra.vv` | Rounded arithmetic scaling right shift. |
| `vssra.vx` | Rounded arithmetic scaling right shift. |
| `vssra.vi` | Rounded arithmetic scaling right shift. |
| `vnsrl.wv` | Narrow double-width source by logical right shift. |
| `vnsrl.wx` | Narrow double-width source by logical right shift. |
| `vnsrl.wi` | Narrow double-width source by logical right shift. |
| `vnsra.wv` | Narrow double-width source by arithmetic right shift. |
| `vnsra.wx` | Narrow double-width source by arithmetic right shift. |
| `vnsra.wi` | Narrow double-width source by arithmetic right shift. |
| `vnclipu.wv` | Round and saturate wider unsigned input to SEW. |
| `vnclipu.wx` | Round and saturate wider unsigned input to SEW. |
| `vnclipu.wi` | Round and saturate wider unsigned input to SEW. |
| `vnclip.wv` | Round and saturate wider signed input to SEW. |
| `vnclip.wx` | Round and saturate wider signed input to SEW. |
| `vnclip.wi` | Round and saturate wider signed input to SEW. |
| `vmul.vv` | Low product bits. |
| `vmul.vx` | Low product bits. |
| `vmulh.vv` | High signed × signed product. |
| `vmulh.vx` | High signed × signed product. |
| `vmulhu.vv` | High unsigned × unsigned product. |
| `vmulhu.vx` | High unsigned × unsigned product. |
| `vmulhsu.vv` | High signed vs2 × unsigned second operand product. |
| `vmulhsu.vx` | High signed vs2 × unsigned second operand product. |
| `vdiv.vv` | Signed quotient with architectural exceptional operands. |
| `vdiv.vx` | Signed quotient with architectural exceptional operands. |
| `vdivu.vv` | Unsigned quotient. |
| `vdivu.vx` | Unsigned quotient. |
| `vrem.vv` | Signed remainder. |
| `vrem.vx` | Signed remainder. |
| `vremu.vv` | Unsigned remainder. |
| `vremu.vx` | Unsigned remainder. |
| `vmadd.vv` | vd = operand1 × old vd + vs2. |
| `vmadd.vx` | vd = operand1 × old vd + vs2. |
| `vnmsub.vv` | vd = −operand1 × old vd + vs2. |
| `vnmsub.vx` | vd = −operand1 × old vd + vs2. |
| `vmacc.vv` | vd = operand1 × vs2 + old vd. |
| `vmacc.vx` | vd = operand1 × vs2 + old vd. |
| `vnmsac.vv` | vd = −operand1 × vs2 + old vd. |
| `vnmsac.vx` | vd = −operand1 × vs2 + old vd. |
| `vwaddu.vv` | Zero-extend operands and add into double-width destination. |
| `vwaddu.vx` | Zero-extend operands and add into double-width destination. |
| `vwaddu.wv` | Zero-extend operands and add into double-width destination. |
| `vwaddu.wx` | Zero-extend operands and add into double-width destination. |
| `vwadd.vv` | Sign-extend operands and add into double-width destination. |
| `vwadd.vx` | Sign-extend operands and add into double-width destination. |
| `vwadd.wv` | Sign-extend operands and add into double-width destination. |
| `vwadd.wx` | Sign-extend operands and add into double-width destination. |
| `vwsubu.vv` | Widening unsigned subtraction. |
| `vwsubu.vx` | Widening unsigned subtraction. |
| `vwsubu.wv` | Widening unsigned subtraction. |
| `vwsubu.wx` | Widening unsigned subtraction. |
| `vwsub.vv` | Widening signed subtraction. |
| `vwsub.vx` | Widening signed subtraction. |
| `vwsub.wv` | Widening signed subtraction. |
| `vwsub.wx` | Widening signed subtraction. |
| `vwmulu.vv` | Full widening unsigned product. |
| `vwmulu.vx` | Full widening unsigned product. |
| `vwmul.vv` | Full widening signed product. |
| `vwmul.vx` | Full widening signed product. |
| `vwmulsu.vv` | Full signed vs2 × unsigned second operand product. |
| `vwmulsu.vx` | Full signed vs2 × unsigned second operand product. |
| `vwmaccu.vv` | Widening unsigned product accumulated into wide vd. |
| `vwmaccu.vx` | Widening unsigned product accumulated into wide vd. |
| `vwmacc.vv` | Widening signed product accumulated into wide vd. |
| `vwmacc.vx` | Widening signed product accumulated into wide vd. |
| `vwmaccsu.vv` | Widening signed/unsigned multiply accumulate. |
| `vwmaccsu.vx` | Widening signed/unsigned multiply accumulate. |
| `vwmaccus.vx` | Widening unsigned vector × signed scalar accumulate. |

### CSR effects

Uses vl/vtype/vstart and masking. Fixed-point rounding uses vxrm; saturation ORs vxsat. No additional CSRs.

[Back to contents](#contents)

## V — permutations masks and reductions

These operations move data or combine lanes; they are not ordinary independent-lane arithmetic. Source/destination overlap and restart restrictions matter. VRGATHEREI16 shares a funct6 slot with slide-up, distinguished by funct3.

Implementation: [src/extensions/ext_v_perm.cpp](../src/extensions/ext_v_perm.cpp), [src/extensions/ext_v_mask.cpp](../src/extensions/ext_v_mask.cpp), [src/extensions/ext_v_reduce.cpp](../src/extensions/ext_v_reduce.cpp), [src/extensions/ext_v.cpp](../src/extensions/ext_v.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `vrgather.vv` | Select source by element index; out-of-range source index yields zero. |
| `vrgather.vx` | Select source by element index; out-of-range source index yields zero. |
| `vrgather.vi` | Select source by element index; out-of-range source index yields zero. |
| `vrgatherei16.vv` | Gather using 16-bit vector indices; funct6=0x0e with OPIVV distinguishes it from scalar/immediate slide-up. |
| `vslideup.vx` | Move source elements to higher indices; low destinations are preserved. |
| `vslideup.vi` | Move source elements to higher indices; low destinations are preserved. |
| `vslidedown.vx` | Move source elements to lower indices; beyond source range yields zero. |
| `vslidedown.vi` | Move source elements to lower indices; beyond source range yields zero. |
| `vslide1up.vx` | Shift up one and insert integer scalar at element zero. |
| `vslide1down.vx` | Shift down one and insert integer scalar at the last active element. |
| `vcompress.vm` | Pack elements selected by a mask into consecutive destination elements. |
| `vmv1r.v` | Copy 1 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv2r.v` | Copy 2 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv4r.v` | Copy 4 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv8r.v` | Copy 8 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv.x.s` | Sign-extend vector element 0 to integer destination. |
| `vmv.s.x` | Copy integer scalar into vector element 0. |
| `vcpop.m` | Count set mask bits over active range into integer destination. |
| `vfirst.m` | Index of first set mask bit, or -1 if none. |
| `vmsbf.m` | Mask of positions before the first set source bit. |
| `vmsif.m` | Mask through and including the first set source bit. |
| `vmsof.m` | Mask selecting only the first set source bit. |
| `viota.m` | Prefix count of set source-mask bits. |
| `vid.v` | Write each element index. |
| `vmand.mm` | Boolean mask operation: a AND b. |
| `vmandn.mm` | Boolean mask operation: a AND NOT b. |
| `vmor.mm` | Boolean mask operation: a OR b. |
| `vmorn.mm` | Boolean mask operation: a OR NOT b. |
| `vmxor.mm` | Boolean mask operation: a XOR b. |
| `vmxnor.mm` | Boolean mask operation: NOT(a XOR b). |
| `vmnand.mm` | Boolean mask operation: NOT(a AND b). |
| `vmnor.mm` | Boolean mask operation: NOT(a OR b). |
| `vzext.vf2` | Zero-extend elements from SEW/2 to SEW; source width must remain supported. |
| `vsext.vf2` | Sign-extend elements from SEW/2 to SEW; source width must remain supported. |
| `vzext.vf4` | Zero-extend elements from SEW/4 to SEW; source width must remain supported. |
| `vsext.vf4` | Sign-extend elements from SEW/4 to SEW; source width must remain supported. |
| `vzext.vf8` | Zero-extend elements from SEW/8 to SEW; source width must remain supported. |
| `vsext.vf8` | Sign-extend elements from SEW/8 to SEW; source width must remain supported. |
| `vredsum.vs` | Reduce active vs2 elements with scalar seed vs1[0] using sum; place result in vd[0]. |
| `vredand.vs` | Reduce active vs2 elements with scalar seed vs1[0] using bitwise AND; place result in vd[0]. |
| `vredor.vs` | Reduce active vs2 elements with scalar seed vs1[0] using bitwise OR; place result in vd[0]. |
| `vredxor.vs` | Reduce active vs2 elements with scalar seed vs1[0] using bitwise XOR; place result in vd[0]. |
| `vredminu.vs` | Reduce active vs2 elements with scalar seed vs1[0] using unsigned minimum; place result in vd[0]. |
| `vredmin.vs` | Reduce active vs2 elements with scalar seed vs1[0] using signed minimum; place result in vd[0]. |
| `vredmaxu.vs` | Reduce active vs2 elements with scalar seed vs1[0] using unsigned maximum; place result in vd[0]. |
| `vredmax.vs` | Reduce active vs2 elements with scalar seed vs1[0] using signed maximum; place result in vd[0]. |
| `vwredsumu.vs` | Reduce active vs2 elements with scalar seed vs1[0] using widening unsigned sum; place result in vd[0]. |
| `vwredsum.vs` | Reduce active vs2 elements with scalar seed vs1[0] using widening signed sum; place result in vd[0]. |

### CSR effects

Uses existing vector state; scalar-result forms write x registers. No new CSR.

[Back to contents](#contents)

## V — floating-point instructions

Ordinary vector FP uses SEW 32/64. SEW=16 admits only two Zvfhmin conversion subcodes; other half operations return without doing arithmetic. The executor, not the display-name table, determines dispatch: several displayed funct6 labels disagree with execution cases. These tables explain the architectural names; they do not assert that the current decoder rejects every non-architectural operand form.

Implementation: [src/extensions/ext_v_fp.cpp](../src/extensions/ext_v_fp.cpp), [src/extensions/ext_v.cpp](../src/extensions/ext_v.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `vfadd.vv` | Floating add. |
| `vfadd.vf` | Floating add. |
| `vfsub.vv` | vs2 minus operand. |
| `vfsub.vf` | vs2 minus operand. |
| `vfrsub.vf` | Scalar minus vector. |
| `vfmul.vv` | Floating multiply. |
| `vfmul.vf` | Floating multiply. |
| `vfdiv.vv` | Vector divided by operand. |
| `vfdiv.vf` | Vector divided by operand. |
| `vfrdiv.vf` | Scalar divided by vector. |
| `vfmin.vv` | Numeric floating minimum. |
| `vfmin.vf` | Numeric floating minimum. |
| `vfmax.vv` | Numeric floating maximum. |
| `vfmax.vf` | Numeric floating maximum. |
| `vfsgnj.vv` | Copy second operand sign. |
| `vfsgnj.vf` | Copy second operand sign. |
| `vfsgnjn.vv` | Copy inverted second operand sign. |
| `vfsgnjn.vf` | Copy inverted second operand sign. |
| `vfsgnjx.vv` | XOR signs. |
| `vfsgnjx.vf` | XOR signs. |
| `vmfeq.vv` | Equality mask. |
| `vmfeq.vf` | Equality mask. |
| `vmfne.vv` | Inequality mask, including unordered NaN cases. |
| `vmfne.vf` | Inequality mask, including unordered NaN cases. |
| `vmflt.vv` | Less-than mask. |
| `vmflt.vf` | Less-than mask. |
| `vmfle.vv` | Less-or-equal mask. |
| `vmfle.vf` | Less-or-equal mask. |
| `vmfgt.vf` | Greater-than mask. |
| `vmfge.vf` | Greater-or-equal mask. |
| `vfmerge.vfm` | Select scalar or vector with mask. |
| `vfmv.v.f` | Broadcast FP scalar. |
| `vfslide1up.vf` | Slide up one and insert FP scalar. |
| `vfslide1down.vf` | Slide down one and insert FP scalar. |
| `vfsqrt.v` | Square root per active element. |
| `vfclass.v` | FP classification mask per element. |
| `vfrec7.v` | Architecturally specified 7-bit reciprocal estimate; DoomV computes a division approximation instead, so bit-exact conformance is not established. |
| `vfrsqrt7.v` | Architecturally specified reciprocal-square-root estimate; DoomV uses square roots and division rather than the prescribed estimate table. |
| `vfmv.f.s` | Move vector element 0 into an FP scalar. |
| `vfmv.s.f` | Move FP scalar into vector element 0. |
| `vfmadd.vv` | Fused expression a×old vd+vs2, with one arithmetic rounding. |
| `vfmadd.vf` | Fused expression a×old vd+vs2, with one arithmetic rounding. |
| `vfnmadd.vv` | Fused expression −a×old vd−vs2, with one arithmetic rounding. |
| `vfnmadd.vf` | Fused expression −a×old vd−vs2, with one arithmetic rounding. |
| `vfmsub.vv` | Fused expression a×old vd−vs2, with one arithmetic rounding. |
| `vfmsub.vf` | Fused expression a×old vd−vs2, with one arithmetic rounding. |
| `vfnmsub.vv` | Fused expression −a×old vd+vs2, with one arithmetic rounding. |
| `vfnmsub.vf` | Fused expression −a×old vd+vs2, with one arithmetic rounding. |
| `vfmacc.vv` | Fused expression a×vs2+old vd, with one arithmetic rounding. |
| `vfmacc.vf` | Fused expression a×vs2+old vd, with one arithmetic rounding. |
| `vfnmacc.vv` | Fused expression −a×vs2−old vd, with one arithmetic rounding. |
| `vfnmacc.vf` | Fused expression −a×vs2−old vd, with one arithmetic rounding. |
| `vfmsac.vv` | Fused expression a×vs2−old vd, with one arithmetic rounding. |
| `vfmsac.vf` | Fused expression a×vs2−old vd, with one arithmetic rounding. |
| `vfnmsac.vv` | Fused expression −a×vs2+old vd, with one arithmetic rounding. |
| `vfnmsac.vf` | Fused expression −a×vs2+old vd, with one arithmetic rounding. |
| `vfwadd.vv` | Widening add; .w form has an already-wide vs2. |
| `vfwadd.vf` | Widening add; .w form has an already-wide vs2. |
| `vfwadd.wv` | Widening add; .w form has an already-wide vs2. |
| `vfwadd.wf` | Widening add; .w form has an already-wide vs2. |
| `vfwsub.vv` | Widening subtract; .w form has an already-wide vs2. |
| `vfwsub.vf` | Widening subtract; .w form has an already-wide vs2. |
| `vfwsub.wv` | Widening subtract; .w form has an already-wide vs2. |
| `vfwsub.wf` | Widening subtract; .w form has an already-wide vs2. |
| `vfwmul.vv` | Widening multiply. |
| `vfwmul.vf` | Widening multiply. |
| `vfwmacc.vv` | Wide a×b+old vd. |
| `vfwmacc.vf` | Wide a×b+old vd. |
| `vfwnmacc.vv` | Wide −a×b−old vd. |
| `vfwnmacc.vf` | Wide −a×b−old vd. |
| `vfwmsac.vv` | Wide a×b−old vd. |
| `vfwmsac.vf` | Wide a×b−old vd. |
| `vfwnmsac.vv` | Wide −a×b+old vd. |
| `vfwnmsac.vf` | Wide −a×b+old vd. |
| `vfredusum.vs` | Unordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfredosum.vs` | Ordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfredmin.vs` | Minimum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfredmax.vs` | Maximum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfwredusum.vs` | Widening unordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfwredosum.vs` | Widening ordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfcvt.xu.f.v` | Floating to unsigned integer; preserve element width. |
| `vfcvt.x.f.v` | Floating to signed integer; preserve element width. |
| `vfcvt.f.xu.v` | Unsigned integer to floating; preserve element width. |
| `vfcvt.f.x.v` | Signed integer to floating; preserve element width. |
| `vfcvt.rtz.xu.f.v` | Floating to unsigned integer, truncating toward zero; preserve element width. |
| `vfcvt.rtz.x.f.v` | Floating to signed integer, truncating toward zero; preserve element width. |
| `vfwcvt.xu.f.v` | Floating to unsigned integer; widen output. |
| `vfwcvt.x.f.v` | Floating to signed integer; widen output. |
| `vfwcvt.f.xu.v` | Unsigned integer to floating; widen output. |
| `vfwcvt.f.x.v` | Signed integer to floating; widen output. |
| `vfwcvt.rtz.xu.f.v` | Floating to unsigned integer, truncating toward zero; widen output. |
| `vfwcvt.rtz.x.f.v` | Floating to signed integer, truncating toward zero; widen output. |
| `vfncvt.xu.f.w` | Floating to unsigned integer; narrow output. |
| `vfncvt.x.f.w` | Floating to signed integer; narrow output. |
| `vfncvt.f.xu.w` | Unsigned integer to floating; narrow output. |
| `vfncvt.f.x.w` | Signed integer to floating; narrow output. |
| `vfncvt.rtz.xu.f.w` | Floating to unsigned integer, truncating toward zero; narrow output. |
| `vfncvt.rtz.x.f.w` | Floating to signed integer, truncating toward zero; narrow output. |
| `vfwcvt.f.f.v` | Widen floating format; includes the half-to-single Zvfhmin case. |
| `vfncvt.f.f.w` | Narrow floating format under frm; includes single-to-half Zvfhmin. |
| `vfncvt.rod.f.f.w` | Narrow with round-to-odd. Present in wider conversion code; current SEW=16 admission gate excludes this subcode. |

### CSR effects

Shares fflags/frm/fcsr plus V CSRs and FS/VS status. No new FP-vector CSR.

[Back to contents](#contents)

## Zvfhmin — minimal vector half conversion

No independent flag; implemented through V floating conversions. It does not enable full half arithmetic or Zvfh.

Implementation: [src/extensions/ext_v_fp.cpp](../src/extensions/ext_v_fp.cpp), [src/extensions/ext_fp16.hpp](../src/extensions/ext_fp16.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `vfwcvt.f.f.v` | At SEW=16, widen binary16 source to binary32 destination. |
| `vfncvt.f.f.w` | At SEW=16, narrow binary32 source to binary16 destination with rounding. |

### CSR effects

Shares floating/vector CSRs. No new CSR.

[Back to contents](#contents)

## Zvbb — vector bit manipulation

No Zvbb runtime flag: V routes these slots directly. Unary slots share funct6 with V sign/zero extension, distinguished by rs1 subcode.

Implementation: [src/extensions/ext_zvbb.cpp](../src/extensions/ext_zvbb.cpp), [src/extensions/ext_v.cpp](../src/extensions/ext_v.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `vandn.vv` | Bitwise vs2 AND NOT operand. |
| `vandn.vx` | Bitwise vs2 AND NOT operand. |
| `vror.vv` | Rotate elements right; immediate may use six bits split across instruction fields. |
| `vror.vx` | Rotate elements right; immediate may use six bits split across instruction fields. |
| `vror.vi` | Rotate elements right; immediate may use six bits split across instruction fields. |
| `vrol.vv` | Rotate elements left. |
| `vrol.vx` | Rotate elements left. |
| `vwsll.vv` | Zero-extend source then shift into double-width destination. |
| `vwsll.vx` | Zero-extend source then shift into double-width destination. |
| `vwsll.vi` | Zero-extend source then shift into double-width destination. |
| `vbrev8.v` | Reverse bits independently in each byte. |
| `vrev8.v` | Reverse bytes within each element. |
| `vbrev.v` | Reverse all bits in each element. |
| `vclz.v` | Count leading zeros per element. |
| `vctz.v` | Count trailing zeros per element. |
| `vcpop.v` | Count set bits per element. |

### CSR effects

Existing V state only; no new CSR.

[Back to contents](#contents)

## Machine privilege — traps and control

Reset is M mode, virtualization false, with generic CSR storage zeroed. Trap entry records PC/cause/tval, saves interrupt enable into previous-enable state, records old privilege, disables the destination global interrupt enable and selects its direct vector. ECALL does not advance epc; the handler must advance it before returning when appropriate.

Important source limitations: MRET/SRET do not comprehensively validate caller privilege or TW/TSR restrictions; SRET directly reads machine status and sepc even in virtual execution. Unknown SYSTEM immediates may advance PC without trapping. These paths must not be described as a complete privileged implementation.

Implementation: [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp), [src/registers.cpp](../src/registers.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `mret` | Restore privilege from MPP, interrupt-enable from MPIE and PC from mepc; restore H virtualization from MPV when applicable. |
| `wfi` | Return immediately and check interrupts again on a following step. The implementation does not comprehensively enforce privilege/TW/virtual restrictions. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `mstatus` | `0x300` | Machine status: interrupt enable/previous enable, previous privilege, MPRV, SUM/MXR, TVM, FS/VS and H-related fields. Generic writable storage is not full WARL enforcement. |
| `misa` | `0x301` | Computed XLEN and I/M/A/C/F/D/V/H bits plus unconditional S/U; writes do not change configuration. |
| `medeleg` | `0x302` | Exception delegation from below M to S/HS; M-origin traps stay in M. |
| `mideleg` | `0x303` | Interrupt delegation; also masks the S-mode interrupt aliases. |
| `mie` | `0x304` | Per-cause interrupt enables. |
| `mtvec` | `0x305` | Trap base; MODE bits cleared on writes, so direct mode only. |
| `mscratch` | `0x340` | Firmware scratch storage, e.g. pointer used when swapping a register on trap entry. |
| `mepc` | `0x341` | PC saved on M-mode trap, consumed by MRET. |
| `mcause` | `0x342` | Cause number plus interrupt indicator for M trap. |
| `mtval` | `0x343` | Fault address or instruction-related trap value. |
| `mip` | `0x344` | Effective pending causes computed from timer, IMSIC and selected software shadow bits. |
| `menvcfg` | `0x30A` | Environment fields used by Sstc, PBMT and pointer masking; not every standardized field is modeled. |
| `mcounteren` | `0x306` | Lower-mode access permissions for unprivileged counters. |
| `mvendorid` | `0xF11` | Generic reset-zero read-only-by-address storage; no specific vendor identity supplied. |
| `marchid` | `0xF12` | Generic reset-zero architecture identity. |
| `mimpid` | `0xF13` | Generic reset-zero implementation identity. |
| `mhartid` | `0xF14` | Generic reset-zero value agrees with the single hart 0 platform. |


[Back to contents](#contents)

## Supervisor privilege and Sv39

S mode owns process translation and ordinary OS trap handling; U-mode application accesses are constrained by PTE permissions and PMP. Sv39 uses 3 levels of 512 eight-byte PTEs and 4-KiB pages, with 2-MiB/1-GiB aligned superpage leaves. Upper virtual bits must sign-extend bit 38. Read/write/execute, U, A and D bits are checked. SUM permits certain S accesses to U pages; MXR allows loads from executable pages. Every walk rereads guest RAM; no TLB exists. RV32 execution support does not imply Sv32 paging: the CSR/walker paths here are RV64/Sv39-shaped.

Implementation: [src/mmu.cpp](../src/mmu.cpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `sret` | Restore supervisor interrupt state and previous privilege and jump to sepc; HS may restore virtualization from hstatus.SPV. See limitations in machine privilege. |
| `sfence.vma` | Synchronize page-table updates and local translations by virtual-address/ASID operands. No cached translation exists; DoomV checks U-mode denial and S-mode mstatus.TVM. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `sstatus` | `0x100` | Masked mstatus view. Current mask contains SIE/SPIE/SPP/SUM/MXR, not the full architectural FS/VS/SD view. |
| `sie` | `0x104` | mie intersected with mideleg; writes update delegated enables only. |
| `stvec` | `0x105` | Direct-mode S/HS trap base. |
| `scounteren` | `0x106` | U-mode counter access gate. |
| `senvcfg` | `0x10A` | Supervisor environment storage; PMM used for U-mode pointer masking. |
| `sscratch` | `0x140` | Supervisor scratch register for software trap entry/context bookkeeping. |
| `sepc` | `0x141` | Saved S/HS trap PC, consumed by SRET. |
| `scause` | `0x142` | S/HS cause and interrupt indicator. |
| `stval` | `0x143` | S/HS fault value, usually faulting virtual address. |
| `sip` | `0x144` | Computed mip masked by mideleg; restricted software writes alter shadow state. |
| `satp` | `0x180` | RV64 MODE[63:60], ASID[59:44], root PPN[43:0]; only Bare(0)/Sv39(8) writes accepted. |


[Back to contents](#contents)

## Svinval — fine-grained translation invalidation

Separate invalidations from the surrounding ordering operations. These have no cache work in DoomV but do have U-mode privilege checks.

Implementation: [src/extensions/ext_svinval.cpp](../src/extensions/ext_svinval.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `sinval.vma` | Invalidate the named virtual-address/ASID translation; S-mode TVM causes illegal instruction. |
| `sfence.w.inval` | Order previous page-table writes before subsequent invalidations; not gated by TVM in this helper. |
| `sfence.inval.ir` | Order prior invalidations before later implicit translation reads; not gated by TVM in this helper. |

### CSR effects

No new CSR. Uses existing privilege and mstatus.TVM. H-specific HINVAL forms are not present.

[Back to contents](#contents)

## Svnapot — naturally aligned translation ranges

No new instructions. Gives PTE bit 63 (N) meaning. The implemented supported NAPOT shape is a 64-KiB mapping at the 4-KiB leaf level: virtual low VPN bits supply part of the final PPN. Reserved shapes fault instead of silently aliasing pages. This is page-table NAPOT, distinct from PMP NAPOT regions.

Implementation: [src/mmu.cpp](../src/mmu.cpp).

### CSR effects

No new CSR; changes PTE interpretation under satp/vsatp and the runtime SVNAPOT flag.

[Back to contents](#contents)

## Svpbmt — page-based memory types

No new instructions. Leaf PTE bits 62:61 specify PMA/default(0), non-cacheable(1), or I/O(2); 3 is reserved. Nonzero PBMT requires the enabled extension and menvcfg.PBMTE. With no caches, the data-path distinction is limited, but reserved and permission checks remain observable.

Implementation: [src/mmu.cpp](../src/mmu.cpp).

### CSR effects

Adds meaning to `menvcfg.PBMTE` bit 62 and PTE fields, not a new CSR. H environment interactions are not a complete implementation merely because henvcfg storage exists.

[Back to contents](#contents)

## Svade and Svadu — accessed and dirty policy

Svade behavior is present in the walker without an independent parser flag: a clear A bit faults, and a clear D bit faults on stores/AMOs. The walker does not update these bits. Svadu automatic updates are not implemented; MENVCFG_ADUE is declared but unused. These names distinguish the implemented policy from the alternative.

Implementation: [src/mmu.cpp](../src/mmu.cpp).

### CSR effects

Uses PTE A[6]/D[7]. No dedicated CSR. `menvcfg.ADUE` bit 61 does not select a live update policy in this implementation.

[Back to contents](#contents)

## Smpmp — physical memory protection

Sixteen implemented entries with R/W/X, address mode A[4:3] and lock L[7]. Modes: OFF, TOR, NA4, NAPOT. TOR gets the lower bound from the prior pmpaddr, or zero for entry 0. Lowest matching entry wins. S/U with no match are denied; M bypasses unlocked permission entries but locked entries constrain M too. A locked TOR entry also locks the preceding address that forms its lower bound. The helper is RV64-shaped; odd pmpcfg registers return zero/ignore writes, rather than implementing RV32 packing or necessarily trapping.

PMP denies access with access-fault causes 1/5/7, not page-fault causes 12/13/15. Checks occur for final accesses and page-table accesses. Some vector/H access paths bypass the final wrapper, so enforcement is not uniform across all instruction families.

Implementation: [src/pmp.hpp](../src/pmp.hpp), [src/pmp.cpp](../src/pmp.cpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `pmpcfg0` | `0x3A0` | Eight packed permission/config bytes for entries 0–7. |
| `pmpcfg2` | `0x3A2` | Eight packed bytes for entries 8–15. |
| `pmpaddr0` | `0x3B0` | Entry 0 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr1` | `0x3B1` | Entry 1 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr2` | `0x3B2` | Entry 2 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr3` | `0x3B3` | Entry 3 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr4` | `0x3B4` | Entry 4 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr5` | `0x3B5` | Entry 5 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr6` | `0x3B6` | Entry 6 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr7` | `0x3B7` | Entry 7 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr8` | `0x3B8` | Entry 8 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr9` | `0x3B9` | Entry 9 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr10` | `0x3BA` | Entry 10 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr11` | `0x3BB` | Entry 11 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr12` | `0x3BC` | Entry 12 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr13` | `0x3BD` | Entry 13 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr14` | `0x3BE` | Entry 14 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr15` | `0x3BF` | Entry 15 address encoded in units of four bytes; 54 writable address bits. |

Unimplemented pmpaddr16–63 read zero and ignore writes; higher configuration fields remain zero. This snapshot has no Smepmp mseccfg policy implementation.

[Back to contents](#contents)

## Ssnpm — pointer masking

Supervisor control of pointer masking for lower execution contexts. The single SSNPM flag enables the shared code. PMM values 0/2/3 select PMLEN 0/7/16; reserved value 1 preserves the old legal field on write. Ordinary S accesses select menvcfg; U accesses select senvcfg; HLV/HSV use hstatus.HUPMM. Data addresses have ignored high bits reconstructed by sign extension of the remaining address. Fetch and page-table-generated addresses are not ordinary tagged data pointers. M-mode masking (Smmpm) is not implemented.

Implementation: [src/mmu.cpp](../src/mmu.cpp), [src/extensions/ext_h.cpp](../src/extensions/ext_h.cpp), [src/extensions.cpp](../src/extensions.cpp).

### CSR effects

No new CSR. Uses `menvcfg/senvcfg/henvcfg.PMM[33:32]` storage and `hstatus.HUPMM[49:48]`; not every virtual PMM selection is implemented.

[Back to contents](#contents)

## Smnpm — pointer masking

Machine control of pointer masking for lower execution contexts. The single SSNPM flag enables the shared code. PMM values 0/2/3 select PMLEN 0/7/16; reserved value 1 preserves the old legal field on write. Ordinary S accesses select menvcfg; U accesses select senvcfg; HLV/HSV use hstatus.HUPMM. Data addresses have ignored high bits reconstructed by sign extension of the remaining address. Fetch and page-table-generated addresses are not ordinary tagged data pointers. M-mode masking (Smmpm) is not implemented.

Implementation: [src/mmu.cpp](../src/mmu.cpp), [src/extensions/ext_h.cpp](../src/extensions/ext_h.cpp), [src/extensions.cpp](../src/extensions.cpp).

### CSR effects

No new CSR. Uses `menvcfg/senvcfg/henvcfg.PMM[33:32]` storage and `hstatus.HUPMM[49:48]`; not every virtual PMM selection is implemented.

[Back to contents](#contents)

## Sspm — pointer masking

Repository spelling/alias accepted by its parser for the shared masking mechanism; do not assume every external tool recognizes this token. The single SSNPM flag enables the shared code. PMM values 0/2/3 select PMLEN 0/7/16; reserved value 1 preserves the old legal field on write. Ordinary S accesses select menvcfg; U accesses select senvcfg; HLV/HSV use hstatus.HUPMM. Data addresses have ignored high bits reconstructed by sign extension of the remaining address. Fetch and page-table-generated addresses are not ordinary tagged data pointers. M-mode masking (Smmpm) is not implemented.

Implementation: [src/mmu.cpp](../src/mmu.cpp), [src/extensions/ext_h.cpp](../src/extensions/ext_h.cpp), [src/extensions.cpp](../src/extensions.cpp).

### CSR effects

No new CSR. Uses `menvcfg/senvcfg/henvcfg.PMM[33:32]` storage and `hstatus.HUPMM[49:48]`; not every virtual PMM selection is implemented.

[Back to contents](#contents)

## H — hypervisor extension

H is off by default. Privilege and virtualization are separate: M, HS=(S,V=0), VS=(S,V=1), host U and VU. First-stage guest translation maps GVA→GPA via vsatp; G-stage maps GPA→physical via hgatp. Even the guest page-table accesses themselves need G-stage translation. Guest-page faults use causes 20/21/23, while virtual-instruction faults use 22.

The explicit HLV/HSV load/store helper checks virtual callers and HU, then calls mmu_translate(as_guest=true) directly. Ordinary fetch/load/store calls leave as_guest=false, and mmu_translate does not inspect get_virt; therefore ordinary VS/VU execution is not automatically routed through this guest two-stage path. It does not pass through the final translate_or_trap wrapper. Guest CSR redirection and two-stage walks are real code; complete hypervisor conformance is not established. In particular interrupt injection, virtual xRET, fence interception, guest time offsets and width-specific controls have gaps.

Implementation: [src/extensions/ext_h.cpp](../src/extensions/ext_h.cpp), [src/extensions/ext_h_ldst.cpp](../src/extensions/ext_h_ldst.cpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp), [src/mmu.cpp](../src/mmu.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `hlv.b` | Load 8 bits using guest translation; narrow signed forms sign-extend. |
| `hlv.bu` | Load and zero-extend 8 guest bits. |
| `hsv.b` | Store low 8 bits through guest translation. |
| `hlv.h` | Load 16 bits using guest translation; narrow signed forms sign-extend. |
| `hlv.hu` | Load and zero-extend 16 guest bits. |
| `hsv.h` | Store low 16 bits through guest translation. |
| `hlv.w` | Load 32 bits using guest translation; narrow signed forms sign-extend. |
| `hlv.wu` | Load and zero-extend 32 guest bits. |
| `hsv.w` | Store low 32 bits through guest translation. |
| `hlv.d` | Load 64 bits using guest translation; narrow signed forms sign-extend. |
| `hsv.d` | Store low 64 bits through guest translation. |
| `hlvx.hu` | Load and zero-extend 16 guest bits using execute permission. |
| `hlvx.wu` | Load and zero-extend 32 guest bits using execute permission. |
| `hfence.vvma` | Synchronize guest first-stage translations; current helper immediately advances PC before privilege validation. |
| `hfence.gvma` | Synchronize G-stage translations by guest physical address/VMID; no cached translation exists, and the same early-return limitation applies. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `hstatus` | `0x600` | HS virtualization status: SPV/SPVP record guest origin, HU permits host U guest-memory instructions; GVA and HUPMM used. VSXL hardwired 64, VSBE/VGEIN zero. VTVM/VTW/VTSR storage is not proof all intercepts work. |
| `hedeleg` | `0x602` | Exception delegation from HS to VS, after M delegation; reserved/HS-only exception bits masked. |
| `hideleg` | `0x603` | Guest interrupt delegation with writable-mask filtering. |
| `hie` | `0x604` | Hypervisor interrupt-enable architectural role; generic storage does not implement every hie/mie alias. |
| `htimedelta` | `0x605` | Architectural guest time offset; current counter helper does not add it to time. |
| `hcounteren` | `0x606` | Architectural guest counter gate; used in scountovf filtering, but ordinary counter_permitted omits this gate. |
| `hgeie` | `0x607` | Guest external-interrupt enable role; no guest IMSIC files are modeled. |
| `henvcfg` | `0x60A` | Virtual environment controls; generic fields plus PMM WARL handling, not full field semantics. |
| `htval` | `0x643` | Guest physical fault information, normally GPA >> 2, separate from stval virtual address. |
| `hip` | `0x644` | Architectural guest interrupt pending view; full alias/interrupt-injection behavior is not implemented. |
| `hvip` | `0x645` | Architectural virtual interrupt injection; no comprehensive connection to compute_mip. |
| `htinst` | `0x64A` | Architectural transformed faulting instruction role; no full transformed-instruction implementation. |
| `hgatp` | `0x680` | Second-stage translation root; Bare or Sv39x4. Sv39x4 has a 16-KiB root and 11-bit top index. |
| `hgeip` | `0xE12` | Guest external-interrupt pending role; GEILEN=0, no guest-file delivery. |
| `vsstatus` | `0x200` | Guest supervisor status; S-name CSR instructions redirect here when virtual. |
| `vsie` | `0x204` | Guest interrupt-enable state; S-name CSR instructions redirect here when virtual. |
| `vstvec` | `0x205` | Guest direct-mode trap vector; S-name CSR instructions redirect here when virtual. |
| `vsscratch` | `0x240` | Guest supervisor scratch; S-name CSR instructions redirect here when virtual. |
| `vsepc` | `0x241` | Guest saved trap PC; S-name CSR instructions redirect here when virtual. |
| `vscause` | `0x242` | Guest trap cause; S-name CSR instructions redirect here when virtual. |
| `vstval` | `0x243` | Guest fault value; S-name CSR instructions redirect here when virtual. |
| `vsip` | `0x244` | Guest pending-interrupt state; S-name CSR instructions redirect here when virtual. |
| `vsatp` | `0x280` | Guest first-stage page-table root, Bare/Sv39 WARL; S-name CSR instructions redirect here when virtual. |

H also adds `mstatus.MPV[39]` and `GVA[38]`. RV32 high-half virtualization CSRs are not modeled as a complete RV32 H implementation.

[Back to contents](#contents)

## Sstc — supervisor timer compare

No new instruction and no independent configuration flag. Sstc allows S-mode to program a timer deadline without an SBI call for each rearm. When STCE is enabled, mtime >= stimecmp contributes STIP. The current helper supports RV64 compare state and does not implement the full privilege/virtual timer-control matrix.

Implementation: [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp), [src/timer.cpp](../src/timer.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `stimecmp` | `0x14D` | Supervisor deadline compared against live mtime. |
| `menvcfg.STCE` | `0x30A bit 63` | Enables the modeled supervisor timer comparison. |

`stimecmph`, `vstimecmp` and its high-half counterpart are not implemented as live timer aliases. `mtime/mtimecmp` are MMIO registers, not Sstc CSRs.

[Back to contents](#contents)

## Smaia — advanced interrupt CSRs

No new opcode. This is the implemented AIA CSR subset, backed by the platform IMSIC objects. Top local interrupt and top external identity are different levels of arbitration. Programmable priority arrays, virtual AIA register banks and a full AIA implementation are not supplied merely by these windows.

Implementation: [src/imsic.cpp](../src/imsic.cpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

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

Implementation: [src/imsic.cpp](../src/imsic.cpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

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

Implementation: [src/extensions/ext_sscofpmf.cpp](../src/extensions/ext_sscofpmf.cpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

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

Implementation: [src/extensions/ext_ssstateen.cpp](../src/extensions/ext_ssstateen.cpp), [src/extensions/ext_ssstateen.hpp](../src/extensions/ext_ssstateen.hpp), [src/extensions/ext_zicsr.cpp](../src/extensions/ext_zicsr.cpp).

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

### CSR effects

No additional state.

[Back to contents](#contents)
