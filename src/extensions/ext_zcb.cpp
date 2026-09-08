// Zcb: the compressed bitmanip/memory encodings. Byte and halfword loads and
// stores that base RVC lacks (c.lbu/c.lhu/c.lh/c.sb/c.sh), a compressed
// multiply (c.mul), and a small unary family (c.zext.b/h/w, c.sext.b/h,
// c.not).
//
// Only the *decode* lives here, because Zcb has no execution of its own:
// every one of these is defined as an alias for a standard instruction, so
// each decodes into that instruction's encoding and flows through the
// existing exec_32I / exec_32M / exec_ZBA / exec_ZBB path unchanged. Setting
// instr.ext to the extension that actually executes it also means the alias
// gates correctly -- c.sext.b is unavailable if Zbb is off, which is the
// right answer.
//
// Called from Decoder::decode_compressed(), which owns the quadrant/funct3
// switch; these two entry points cover the slots that switch hands over.
#include "riscv_decoder.hpp"
#include "extensions.hpp"
#include <cstdint>

namespace {

// Same 3-bit-to-x8..x15 mapping the rest of the compressed decoder uses.
// Re-derived here rather than shared, matching this project's convention of
// each decoder extracting the fields it needs for itself.
inline uint8_t creg_prime(uint16_t raw16, int shift) { return (uint8_t)(((raw16 >> shift) & 0x7) + 8); }

} // namespace

// Quadrant 0, funct3=100: the byte/halfword memory accesses.
DecodedInstruction Decoder::decode_zcb_mem(uint16_t raw16) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::C;
	instr.length = 2;
	instr.mnemonic = "???";

	uint8_t sub = (raw16 >> 10) & 0x3;   // funct6's low two bits pick the operation
	uint8_t b6 = (raw16 >> 6) & 0x1;
	uint8_t b5 = (raw16 >> 5) & 0x1;
	uint8_t rs1 = creg_prime(raw16, 7);
	uint8_t rd_rs2 = creg_prime(raw16, 2);

	switch (sub) {
	case 0b00: // C.LBU -> LBU rd', off(rs1'); off[1:0] = {b5, b6}
		instr.mnemonic = "C.LBU";
		instr.opcode = 0b0000011; instr.funct3 = 0b100;
		instr.rd = rd_rs2; instr.rs1 = rs1;
		instr.imm = (int64_t)((b5 << 1) | b6);
		break;
	case 0b01: // b6 picks C.LH (1) vs C.LHU (0); off[1] = b5, always halfword-aligned
		instr.mnemonic = b6 ? "C.LH" : "C.LHU";
		instr.opcode = 0b0000011; instr.funct3 = b6 ? 0b001 : 0b101;
		instr.rd = rd_rs2; instr.rs1 = rs1;
		instr.imm = (int64_t)(b5 << 1);
		break;
	case 0b10: // C.SB -> SB rs2', off(rs1')
		instr.mnemonic = "C.SB";
		instr.opcode = 0b0100011; instr.funct3 = 0b000;
		instr.rs1 = rs1; instr.rs2 = rd_rs2;
		instr.imm = (int64_t)((b5 << 1) | b6);
		break;
	default:  // 0b11 -- C.SH
		instr.mnemonic = "C.SH";
		instr.opcode = 0b0100011; instr.funct3 = 0b001;
		instr.rs1 = rs1; instr.rs2 = rd_rs2;
		instr.imm = (int64_t)(b5 << 1);
		break;
	}

	return instr;
}

// Quadrant 1, funct3=100 with bits[11:10]=11 and bit12=1: the slot base RVC
// uses for C.SUBW/C.ADDW at funct2 00/01, which Zcb extends with C.MUL at 10
// and the unary family at 11.
DecodedInstruction Decoder::decode_zcb_alu(uint16_t raw16) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::C;
	instr.length = 2;
	instr.mnemonic = "???";

	uint8_t rd = creg_prime(raw16, 7);
	uint8_t rs2 = creg_prime(raw16, 2);
	uint8_t funct2 = (raw16 >> 5) & 0x3;

	if (funct2 == 0b10) { // C.MUL rd',rd',rs2' -> MUL
		instr.ext = Extension::M;
		instr.mnemonic = "C.MUL";
		instr.opcode = 0b0110011; instr.funct3 = 0b000; instr.funct7 = 0b0000001;
		instr.rd = rd; instr.rs1 = rd; instr.rs2 = rs2;
		return instr;
	}

	// funct2 == 0b11 -- the unary family, selected by bits[4:2]. The two that
	// are plain masking (zext.b, not) stay in base I; the rest borrow Zbb/Zba
	// and gate on those accordingly.
	switch ((raw16 >> 2) & 0x7) {
	case 0b000: // C.ZEXT.B -> ANDI rd,rd,255
		instr.mnemonic = "C.ZEXT.B";
		instr.opcode = 0b0010011; instr.funct3 = 0b111;
		instr.rd = rd; instr.rs1 = rd; instr.imm = 255;
		break;
	case 0b001: // C.SEXT.B -> Zbb sext.b
		instr.ext = Extension::ZBB; instr.mnemonic = "C.SEXT.B";
		instr.opcode = 0b0010011; instr.funct3 = 0b001; instr.funct7 = 0b0110000;
		instr.rd = rd; instr.rs1 = rd; instr.rs2 = 4; instr.imm = 4;
		break;
	case 0b010: // C.ZEXT.H -> Zbb zext.h (OP-32 form on RV64)
		instr.ext = Extension::ZBB; instr.mnemonic = "C.ZEXT.H";
		instr.opcode = 0b0111011; instr.funct3 = 0b100; instr.funct7 = 0b0000100;
		instr.rd = rd; instr.rs1 = rd; instr.rs2 = 0; instr.word_op = true;
		break;
	case 0b011: // C.SEXT.H -> Zbb sext.h
		instr.ext = Extension::ZBB; instr.mnemonic = "C.SEXT.H";
		instr.opcode = 0b0010011; instr.funct3 = 0b001; instr.funct7 = 0b0110000;
		instr.rd = rd; instr.rs1 = rd; instr.rs2 = 5; instr.imm = 5;
		break;
	case 0b100: // C.ZEXT.W -> Zba add.uw rd,rd,x0 (RV64 only)
		if (!Extensions.XLEN64) { instr.ext = Extension::ILLEGAL; break; }
		instr.ext = Extension::ZBA; instr.mnemonic = "C.ZEXT.W";
		instr.opcode = 0b0111011; instr.funct3 = 0b000; instr.funct7 = 0b0000100;
		instr.rd = rd; instr.rs1 = rd; instr.rs2 = 0; instr.word_op = true;
		break;
	case 0b101: // C.NOT -> XORI rd,rd,-1
		instr.mnemonic = "C.NOT";
		instr.opcode = 0b0010011; instr.funct3 = 0b100;
		instr.rd = rd; instr.rs1 = rd; instr.imm = -1;
		break;
	default:
		instr.ext = Extension::ILLEGAL; // 110/111 reserved
		break;
	}

	return instr;
}
