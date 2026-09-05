// Zbb: basic bit manipulation. Negated logic (andn/orn/xnor), bit counting
// (clz/ctz/cpop and their W forms), signed and unsigned min/max, sign- and
// zero-extension, rotates, orc.b and rev8.
//
// Shares OP/OP-32/OP-IMM/OP-IMM-32 with base I and the other bitmanip
// families, split by funct7 (or funct6, where a 6-bit shamt claims funct7's
// low bit) in Decoder::classify().
//
// The counting helpers are written as plain loops rather than compiler
// builtins so the zero cases are explicit: clz/ctz of 0 is the full width,
// which is exactly the boundary an off-by-one hides in.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

namespace {

// RV64 rotates are modulo 64; the W forms rotate within the low 32 bits and
// sign-extend the result back out.
inline uint64_t rotr64(uint64_t v, unsigned n) { n &= 63; return n ? ((v >> n) | (v << (64 - n))) : v; }
inline uint64_t rotl64(uint64_t v, unsigned n) { n &= 63; return n ? ((v << n) | (v >> (64 - n))) : v; }
inline uint32_t rotr32(uint32_t v, unsigned n) { n &= 31; return n ? ((v >> n) | (v << (32 - n))) : v; }
inline uint32_t rotl32(uint32_t v, unsigned n) { n &= 31; return n ? ((v << n) | (v >> (32 - n))) : v; }

inline uint64_t clz64(uint64_t v) { if (!v) return 64; uint64_t n = 0; while (!(v & (1ull << 63))) { v <<= 1; n++; } return n; }
inline uint64_t ctz64(uint64_t v) { if (!v) return 64; uint64_t n = 0; while (!(v & 1)) { v >>= 1; n++; } return n; }
inline uint64_t cpop64(uint64_t v) { uint64_t n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
inline uint64_t clz32(uint32_t v) { if (!v) return 32; uint64_t n = 0; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
inline uint64_t ctz32(uint32_t v) { if (!v) return 32; uint64_t n = 0; while (!(v & 1)) { v >>= 1; n++; } return n; }

// orc.b: every byte becomes all-ones if any bit in it was set, else zero.
inline uint64_t orcb(uint64_t v)
{
	uint64_t out = 0;
	for (int i = 0; i < 8; i++) {
		uint64_t byte = (v >> (i * 8)) & 0xFF;
		if (byte) out |= 0xFFull << (i * 8);
	}
	return out;
}

// rev8: reverse byte order across the whole register (the RV64 form).
inline uint64_t rev8_64(uint64_t v)
{
	uint64_t out = 0;
	for (int i = 0; i < 8; i++) out |= ((v >> (i * 8)) & 0xFF) << ((7 - i) * 8);
	return out;
}

} // namespace

DecodedInstruction Decoder::decode_zbb(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZBB;
	instr.length = 4;
	instr.mnemonic = "???";

	uint8_t opcode = raw_instr & 0x7F;
	instr.opcode = opcode;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;
	instr.word_op = (opcode == 0b0111011 || opcode == 0b0011011);
	instr.imm = (raw_instr >> 20) & 0x3F; // rori/roriw shamt

	uint8_t funct6 = (raw_instr >> 26) & 0x3F;
	uint8_t funct3 = instr.funct3;
	uint8_t funct7 = instr.funct7;
	uint8_t rs2    = instr.rs2;

	switch (opcode) {
	case 0b0110011: // OP
		if (funct7 == 0b0100000) {
			instr.mnemonic = (funct3 == 0b111) ? "ANDN" : (funct3 == 0b110) ? "ORN" : "XNOR";
		} else if (funct7 == 0b0000101) {
			instr.mnemonic = (funct3 == 0b100) ? "MIN" : (funct3 == 0b101) ? "MINU"
			               : (funct3 == 0b110) ? "MAX" : "MAXU";
		} else { // funct7 == 0b0110000
			instr.mnemonic = (funct3 == 0b001) ? "ROL" : "ROR";
		}
		break;

	case 0b0111011: // OP-32
		if (funct7 == 0b0000100) {
			instr.mnemonic = "ZEXT.H";
		} else { // funct7 == 0b0110000
			instr.mnemonic = (funct3 == 0b001) ? "ROLW" : "RORW";
		}
		break;

	case 0b0010011: // OP-IMM
		if (funct3 == 0b001) { // unary family, selector in the rs2 field
			instr.mnemonic = (rs2 == 0) ? "CLZ" : (rs2 == 1) ? "CTZ" : (rs2 == 2) ? "CPOP"
			               : (rs2 == 4) ? "SEXT.B" : "SEXT.H";
		} else { // funct3 == 0b101
			if (funct6 == 0b011000)      instr.mnemonic = "RORI";
			else if (funct6 == 0b001010) instr.mnemonic = "ORC.B";
			else                         instr.mnemonic = "REV8";
		}
		break;

	case 0b0011011: // OP-IMM-32
		if (funct3 == 0b001) {
			instr.mnemonic = (rs2 == 0) ? "CLZW" : (rs2 == 1) ? "CTZW" : "CPOPW";
		} else {
			instr.mnemonic = "RORIW";
		}
		break;

	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_ZBB(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();
	uint64_t rs1_val = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);
	uint64_t shamt = (uint64_t)instr.imm;
	uint64_t result = 0;

	uint8_t funct3 = instr.funct3;
	uint8_t funct7 = instr.funct7;
	uint8_t funct6 = (uint8_t)(funct7 >> 1);

	switch (instr.opcode) {
	case 0b0110011: // OP
		if (funct7 == 0b0100000) { // andn/orn/xnor
			result = (funct3 == 0b111) ? (rs1_val & ~rs2_val)
			       : (funct3 == 0b110) ? (rs1_val | ~rs2_val)
			                           : ~(rs1_val ^ rs2_val);
		} else if (funct7 == 0b0000101) { // min/minu/max/maxu
			int64_t a = (int64_t)rs1_val, b = (int64_t)rs2_val;
			result = (funct3 == 0b100) ? (uint64_t)((a < b) ? a : b)
			       : (funct3 == 0b101) ? ((rs1_val < rs2_val) ? rs1_val : rs2_val)
			       : (funct3 == 0b110) ? (uint64_t)((a > b) ? a : b)
			                           : ((rs1_val > rs2_val) ? rs1_val : rs2_val);
		} else { // funct7 == 0b0110000 -- rol/ror
			result = (funct3 == 0b001) ? rotl64(rs1_val, (unsigned)rs2_val)
			                           : rotr64(rs1_val, (unsigned)rs2_val);
		}
		break;

	case 0b0111011: // OP-32
		if (funct7 == 0b0000100) { // zext.h -- rs2 is a fixed selector, not an operand
			result = (uint64_t)(uint16_t)rs1_val;
		} else { // rolw/rorw -- 32-bit rotate, sign-extended out
			uint32_t v = (uint32_t)rs1_val;
			uint32_t r = (funct3 == 0b001) ? rotl32(v, (unsigned)rs2_val) : rotr32(v, (unsigned)rs2_val);
			result = sext32(r);
		}
		break;

	case 0b0010011: // OP-IMM
		if (funct3 == 0b001) { // unary family
			switch (instr.rs2) {
			case 0:  result = clz64(rs1_val); break;
			case 1:  result = ctz64(rs1_val); break;
			case 2:  result = cpop64(rs1_val); break;
			case 4:  result = (uint64_t)(int64_t)(int8_t)rs1_val; break;  // sext.b
			default: result = (uint64_t)(int64_t)(int16_t)rs1_val; break; // sext.h
			}
		} else { // funct3 == 0b101
			if (funct6 == 0b011000)      result = rotr64(rs1_val, (unsigned)shamt);
			else if (funct6 == 0b001010) result = orcb(rs1_val);
			else                         result = rev8_64(rs1_val);
		}
		break;

	case 0b0011011: // OP-IMM-32
		if (funct3 == 0b001) { // clzw/ctzw/cpopw -- low 32 bits only
			uint32_t v = (uint32_t)rs1_val;
			result = (instr.rs2 == 0) ? clz32(v) : (instr.rs2 == 1) ? ctz32(v) : cpop64(v);
		} else { // roriw
			result = sext32(rotr32((uint32_t)rs1_val, (unsigned)(shamt & 31)));
		}
		break;

	default:
		break;
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}
