# Integer and compressed instructions

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

Start with registers, arithmetic and branches. Then use atomics for shared state, compressed encodings for code size, and bit manipulation for common integer patterns.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [I — RV32I and RV64I](#i--rv32i-and-rv64i)
- [M — integer multiplication and division](#m--integer-multiplication-and-division)
- [A — atomics and reservations](#a--atomics-and-reservations)
- [C — compressed instructions](#c--compressed-instructions)
- [Zcb — additional compressed operations](#zcb--additional-compressed-operations)
- [Zba — address generation](#zba--address-generation)
- [Zbb — basic bit manipulation](#zbb--basic-bit-manipulation)
- [Zbs — single-bit manipulation](#zbs--single-bit-manipulation)
- [Zicond — conditional zero](#zicond--conditional-zero)

## I — RV32I and RV64I

I supplies control flow, integer computation and memory access. x0 discards writes; other registers are 64-bit host containers even in RV32 mode. Loads/stores call translation and physical-access checking. Ordinary scalar misalignment is handled in software paths rather than modeling a hardware pipeline. ECALL/EBREAK belong to the base architecture but DoomV routes their SYSTEM encodings through its Zicsr execution family, so that switch matters in this implementation.

Implementation: [src/extensions/ext_i.cpp](../../src/extensions/ext_i.cpp), [src/registers.cpp](../../src/registers.cpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

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

</details>

### CSR effects

No dedicated new arithmetic CSR. ECALL/EBREAK update the trap CSRs described under machine/supervisor architecture.

[Back to contents](#contents)

## M — integer multiplication and division

Integer products and quotients use explicit widths and signedness. Division by zero yields an all-ones quotient and the dividend as remainder; signed minimum / -1 yields the minimum and remainder zero. These cases must avoid host C++ division undefined behavior.

Implementation: [src/extensions/ext_m.cpp](../../src/extensions/ext_m.cpp).

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

[Back to contents](#contents)

## A — atomics and reservations

A supplies LR/SC and AMOs. `.aq`, `.rl` and `.aqrl` qualify ordering rather than changing the arithmetic; the interpreter is single-hart and synchronous. Natural alignment is checked and faults are distinct from translation failures. AMO permission needs both read and write. The reservation is internal RiscvCore state, not a CSR and not proof of multi-hart coherence. Zaamo/Zalrsc are useful architectural subset names, but the parser only exposes A here.

Implementation: [src/extensions/ext_a.cpp](../../src/extensions/ext_a.cpp), [src/riscv_core.hpp](../../src/riscv_core.hpp).

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

[Back to contents](#contents)

## C — compressed instructions

Compression changes encodings and operand restrictions, not the underlying arithmetic. Primed integer registers designate x8–x15. Stack-relative forms use x2. The same slot can mean C.JAL on RV32 or C.ADDIW on RV64, and C.FLW versus C.LD. Zca/Zcd/Zcf are not separate runtime switches in this source.

Implementation: [src/extensions/ext_c.cpp](../../src/extensions/ext_c.cpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

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

</details>

### CSR effects

No new CSRs. Floating compressed aliases use F/D state and flags; C.EBREAK changes trap state.

[Back to contents](#contents)

## Zcb — additional compressed operations

No Zcb flag exists. Memory aliases run under C; arithmetic aliases use M, Zba or Zbb where appropriate.

Implementation: [src/extensions/ext_zcb.cpp](../../src/extensions/ext_zcb.cpp), [src/extensions/ext_c.cpp](../../src/extensions/ext_c.cpp).

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

[Back to contents](#contents)

## Zba — address generation

Scaled indexing and unsigned-word address arithmetic.

Implementation: [src/extensions/ext_zba.cpp](../../src/extensions/ext_zba.cpp).

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

[Back to contents](#contents)

## Zbb — basic bit manipulation

Boolean, counting, rotate, min/max and extension operations. The current helper implementation contains 64-bit-specific paths; selecting RV32 should not be taken as proof of complete RV32 Zbb behavior.

Implementation: [src/extensions/ext_zbb.cpp](../../src/extensions/ext_zbb.cpp).

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

[Back to contents](#contents)

## Zbs — single-bit manipulation

Bit indices are operation-width positions, not memory addresses.

Implementation: [src/extensions/ext_zbs.cpp](../../src/extensions/ext_zbs.cpp).

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

[Back to contents](#contents)

## Zicond — conditional zero

Build conditional selection without a branch.

Implementation: [src/extensions/ext_zicond.cpp](../../src/extensions/ext_zicond.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `czero.eqz` | rd=0 when rs2==0, otherwise rd=rs1. |
| `czero.nez` | rd=0 when rs2!=0, otherwise rd=rs1. |

[Back to contents](#contents)
