// Zba/Zbb/Zbs: the bitmanip families. One file rather than three because
// they share encoding space so heavily -- all three scatter themselves
// across OP/OP-32/OP-IMM/OP-IMM-32 as funct7/funct6 sub-cases of the base
// integer opcodes, so splitting them would mean three near-identical
// decoders each re-deriving the same fields. classify() still tags each
// instruction with its own Extension, so they gate independently.
//
// These were originally out of scope (bare-metal Doom never emits them),
// but any modern riscv64 Linux userspace does: the RVA23 profile mandates
// Zba/Zbb/Zbs, so a distro-built glibc uses them unconditionally rather
// than behind a runtime capability check.
//
// The bug that motivated implementing them is worth recording. The R-type
// path in ext_i.cpp only ever inspected funct7 to separate SUB from ADD and
// SRA from SRL; every other funct7 fell through to the base-I meaning. So
// `sh3add a4,a4,a5` (funct3=110, funct7=0010000) silently executed as OR,
// yielding a5|4 instead of (a4<<3)+a5. Inside glibc's memset that produced
// a loop bound the pointer could never equal, and the runaway 8-byte stores
// walked off the end of the stack VMA -- surfacing as a SIGSEGV hundreds of
// instructions later with nothing pointing back at the real cause.
// classify() now returns ILLEGAL for unrecognized funct7/funct6 values
// instead of aliasing them onto base-I ops, so the next missing extension
// halts loudly on its first instruction instead of computing a wrong
// number quietly.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

namespace {

// RV64 rotates are modulo 64; the W-suffixed forms rotate within the low
// 32 bits and sign-extend the result back out.
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

DecodedInstruction Decoder::decode_zb(uint32_t raw_instr, Extension ext) const
{
	DecodedInstruction instr{};
	instr.ext = ext;
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

	uint8_t funct6 = (raw_instr >> 26) & 0x3F;
	uint8_t funct3 = instr.funct3;
	uint8_t funct7 = instr.funct7;
	uint8_t rs2    = instr.rs2;

	// The immediate forms carry a shamt where rs2 sits: 6 bits on RV64,
	// except the OP-IMM-32 unary ops where bit 25 belongs to funct7.
	instr.imm = (raw_instr >> 20) & 0x3F;

	switch (opcode) {
	case 0b0110011: // OP
		if (funct7 == 0b0010000) {
			instr.mnemonic = (funct3 == 0b010) ? "SH1ADD" : (funct3 == 0b100) ? "SH2ADD" : "SH3ADD";
		} else if (funct7 == 0b0100000) {
			instr.mnemonic = (funct3 == 0b111) ? "ANDN" : (funct3 == 0b110) ? "ORN" : "XNOR";
		} else if (funct7 == 0b0000101) {
			instr.mnemonic = (funct3 == 0b100) ? "MIN" : (funct3 == 0b101) ? "MINU"
			               : (funct3 == 0b110) ? "MAX" : "MAXU";
		} else if (funct7 == 0b0110000) {
			instr.mnemonic = (funct3 == 0b001) ? "ROL" : "ROR";
		} else if (funct7 == 0b0100100) {
			instr.mnemonic = (funct3 == 0b001) ? "BCLR" : "BEXT";
		} else if (funct7 == 0b0110100) {
			instr.mnemonic = "BINV";
		} else if (funct7 == 0b0000111) {
			instr.mnemonic = (funct3 == 0b101) ? "CZERO.EQZ" : "CZERO.NEZ";
		} else if (funct7 == 0b0010100) {
			instr.mnemonic = "BSET";
		}
		break;

	case 0b0111011: // OP-32
		if (funct7 == 0b0000100) {
			instr.mnemonic = (funct3 == 0b000) ? "ADD.UW" : "ZEXT.H";
		} else if (funct7 == 0b0010000) {
			instr.mnemonic = (funct3 == 0b010) ? "SH1ADD.UW" : (funct3 == 0b100) ? "SH2ADD.UW" : "SH3ADD.UW";
		} else if (funct7 == 0b0110000) {
			instr.mnemonic = (funct3 == 0b001) ? "ROLW" : "RORW";
		}
		break;

	case 0b0010011: // OP-IMM
		if (funct3 == 0b001) {
			if (funct6 == 0b011000) {
				instr.mnemonic = (rs2 == 0) ? "CLZ" : (rs2 == 1) ? "CTZ" : (rs2 == 2) ? "CPOP"
				               : (rs2 == 4) ? "SEXT.B" : "SEXT.H";
			} else {
				instr.mnemonic = (funct6 == 0b001010) ? "BSETI" : (funct6 == 0b010010) ? "BCLRI" : "BINVI";
			}
		} else { // funct3 == 0b101
			if (funct6 == 0b011000)      instr.mnemonic = "RORI";
			else if (funct6 == 0b010010) instr.mnemonic = "BEXTI";
			else if (funct6 == 0b001010) instr.mnemonic = "ORC.B";
			else                         instr.mnemonic = "REV8";
		}
		break;

	case 0b0011011: // OP-IMM-32
		if (funct3 == 0b001) {
			if (funct6 == 0b000010) {
				instr.mnemonic = "SLLI.UW";
			} else {
				instr.mnemonic = (rs2 == 0) ? "CLZW" : (rs2 == 1) ? "CTZW" : "CPOPW";
			}
		} else {
			instr.mnemonic = "RORIW";
		}
		break;

	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_ZB(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem; // no bitmanip instruction touches memory
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
		if (funct7 == 0b0010000) { // Zba sh{1,2,3}add
			unsigned sh = (funct3 == 0b010) ? 1u : (funct3 == 0b100) ? 2u : 3u;
			result = (rs1_val << sh) + rs2_val;
		} else if (funct7 == 0b0100000) { // Zbb andn/orn/xnor
			result = (funct3 == 0b111) ? (rs1_val & ~rs2_val)
			       : (funct3 == 0b110) ? (rs1_val | ~rs2_val)
			                           : ~(rs1_val ^ rs2_val);
		} else if (funct7 == 0b0000101) { // Zbb min/minu/max/maxu
			int64_t a = (int64_t)rs1_val, b = (int64_t)rs2_val;
			result = (funct3 == 0b100) ? (uint64_t)((a < b) ? a : b)
			       : (funct3 == 0b101) ? ((rs1_val < rs2_val) ? rs1_val : rs2_val)
			       : (funct3 == 0b110) ? (uint64_t)((a > b) ? a : b)
			                           : ((rs1_val > rs2_val) ? rs1_val : rs2_val);
		} else if (funct7 == 0b0110000) { // Zbb rol/ror
			result = (funct3 == 0b001) ? rotl64(rs1_val, (unsigned)rs2_val)
			                           : rotr64(rs1_val, (unsigned)rs2_val);
		} else if (funct7 == 0b0100100) { // Zbs bclr/bext
			unsigned n = (unsigned)(rs2_val & 63);
			result = (funct3 == 0b001) ? (rs1_val & ~(1ull << n)) : ((rs1_val >> n) & 1);
		} else if (funct7 == 0b0000111) { // Zicond: conditionally zero, else pass rs1 through
			result = (funct3 == 0b101) ? ((rs2_val == 0) ? 0 : rs1_val)  // czero.eqz
			                           : ((rs2_val != 0) ? 0 : rs1_val); // czero.nez
		} else if (funct7 == 0b0110100) { // Zbs binv
			result = rs1_val ^ (1ull << (rs2_val & 63));
		} else { // funct7 == 0b0010100 -- Zbs bset
			result = rs1_val | (1ull << (rs2_val & 63));
		}
		break;

	case 0b0111011: // OP-32
		if (funct7 == 0b0000100) {
			// add.uw zero-extends rs1's low half before adding; zext.h
			// keeps the low 16 bits (its rs2 field is a fixed selector,
			// not an operand).
			result = (funct3 == 0b000) ? ((uint64_t)(uint32_t)rs1_val + rs2_val)
			                           : (uint64_t)(uint16_t)rs1_val;
		} else if (funct7 == 0b0010000) { // Zba sh{1,2,3}add.uw
			unsigned sh = (funct3 == 0b010) ? 1u : (funct3 == 0b100) ? 2u : 3u;
			result = ((uint64_t)(uint32_t)rs1_val << sh) + rs2_val;
		} else { // funct7 == 0b0110000 -- Zbb rolw/rorw
			uint32_t v = (uint32_t)rs1_val;
			uint32_t r = (funct3 == 0b001) ? rotl32(v, (unsigned)rs2_val) : rotr32(v, (unsigned)rs2_val);
			result = sext32(r);
		}
		break;

	case 0b0010011: // OP-IMM
		if (funct3 == 0b001) {
			if (funct6 == 0b011000) { // Zbb unary -- selector lives in the rs2 field
				switch (instr.rs2) {
				case 0:  result = clz64(rs1_val); break;
				case 1:  result = ctz64(rs1_val); break;
				case 2:  result = cpop64(rs1_val); break;
				case 4:  result = (uint64_t)(int64_t)(int8_t)rs1_val; break;  // sext.b
				default: result = (uint64_t)(int64_t)(int16_t)rs1_val; break; // sext.h
				}
			} else { // Zbs bseti/bclri/binvi
				unsigned n = (unsigned)(shamt & 63);
				result = (funct6 == 0b001010) ? (rs1_val | (1ull << n))
				       : (funct6 == 0b010010) ? (rs1_val & ~(1ull << n))
				                              : (rs1_val ^ (1ull << n));
			}
		} else { // funct3 == 0b101
			if (funct6 == 0b011000)      result = rotr64(rs1_val, (unsigned)shamt);
			else if (funct6 == 0b010010) result = (rs1_val >> (shamt & 63)) & 1; // bexti
			else if (funct6 == 0b001010) result = orcb(rs1_val);
			else                         result = rev8_64(rs1_val);
		}
		break;

	case 0b0011011: // OP-IMM-32
		if (funct3 == 0b001) {
			if (funct6 == 0b000010) { // Zba slli.uw -- zero-extend first, then shift in 64 bits
				result = (uint64_t)(uint32_t)rs1_val << (shamt & 63);
			} else { // Zbb clzw/ctzw/cpopw
				uint32_t v = (uint32_t)rs1_val;
				result = (instr.rs2 == 0) ? clz32(v) : (instr.rs2 == 1) ? ctz32(v) : cpop64(v);
			}
		} else { // Zbb roriw
			result = sext32(rotr32((uint32_t)rs1_val, (unsigned)(shamt & 31)));
		}
		break;

	default:
		break;
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}
