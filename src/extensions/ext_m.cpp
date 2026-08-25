// M extension: integer multiply/divide/remainder. Shares its opcode
// encoding space with I (OP/OP-32) -- Decoder::decode() routes here only
// when classify() has already determined the M-side split via funct7.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_m(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::M;
	instr.length = 4;
	instr.mnemonic = "???";

	uint8_t opcode = raw_instr & 0x7F;
	uint8_t rd     = (raw_instr >> 7) & 0x1F;
	uint8_t funct3 = (raw_instr >> 12) & 0x07;
	uint8_t rs1    = (raw_instr >> 15) & 0x1F;
	uint8_t rs2    = (raw_instr >> 20) & 0x1F;
	uint8_t funct7 = (raw_instr >> 25) & 0x7F;

	instr.opcode = opcode;
	instr.rd = rd;
	instr.rs1 = rs1;
	instr.rs2 = rs2;
	instr.funct3 = funct3;
	instr.funct7 = funct7;

	if (opcode == 0b0111011) { // OP-32 (RV64), M-side: MULW/DIVW/DIVUW/REMW/REMUW
		instr.word_op = true;
		switch (funct3) {
		case 0b000: instr.mnemonic = "MULW";  break;
		case 0b100: instr.mnemonic = "DIVW";  break;
		case 0b101: instr.mnemonic = "DIVUW"; break;
		case 0b110: instr.mnemonic = "REMW";  break;
		case 0b111: instr.mnemonic = "REMUW"; break;
		}
	} else { // OP, M-side: MUL/MULH/MULHSU/MULHU/DIV/DIVU/REM/REMU
		switch (funct3) {
		case 0b000: instr.mnemonic = "MUL";    break;
		case 0b001: instr.mnemonic = "MULH";   break;
		case 0b010: instr.mnemonic = "MULHSU"; break;
		case 0b011: instr.mnemonic = "MULHU";  break;
		case 0b100: instr.mnemonic = "DIV";    break;
		case 0b101: instr.mnemonic = "DIVU";   break;
		case 0b110: instr.mnemonic = "REM";    break;
		case 0b111: instr.mnemonic = "REMU";   break;
		}
	}

	return instr;
}

void RiscvCore::exec_32M(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();

	uint64_t rs1_val = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);
	int64_t rs1_s = (int64_t)rs1_val;
	int64_t rs2_s = (int64_t)rs2_val;

	uint64_t result = 0;

	if (instr.word_op) {
		// MULW/DIVW/DIVUW/REMW/REMUW: operate on the low 32 bits, sign-
		// extend the 32-bit result to 64 -- including DIVUW/REMUW, which
		// despite being unsigned math still sign-extend per the W-suffix
		// convention.
		int32_t a = (int32_t)rs1_val, b = (int32_t)rs2_val;
		uint32_t ua = (uint32_t)rs1_val, ub = (uint32_t)rs2_val;
		uint32_t result32 = 0;
		switch (instr.funct3) {
		case 0b000: result32 = (uint32_t)(a * b); break; // MULW
		case 0b100: // DIVW
			if (b == 0) result32 = 0xFFFFFFFF;
			else if (a == INT32_MIN && b == -1) result32 = (uint32_t)INT32_MIN;
			else result32 = (uint32_t)(a / b);
			break;
		case 0b101: // DIVUW
			result32 = (ub == 0) ? 0xFFFFFFFF : (ua / ub);
			break;
		case 0b110: // REMW
			if (b == 0) result32 = (uint32_t)a;
			else if (a == INT32_MIN && b == -1) result32 = 0;
			else result32 = (uint32_t)(a % b);
			break;
		case 0b111: // REMUW
			result32 = (ub == 0) ? ua : (ua % ub);
			break;
		}
		result = sext32(result32);
	} else if (!Extensions.XLEN64) {
		// RV32: M's base instructions are inherently 32-bit -- there's no
		// separate OP-32 opcode in RV32 at all -- so this is the same
		// "compute low 32, sign-extend" shape as the word_op branch above,
		// just covering the full 8-instruction set. RV64's W-suffix group
		// only has 5 (no MULH/MULHSU/MULHU): a 32x32 multiply's high half
		// is redundant to expose separately once XLEN is already 64, since
		// the full 64-bit product is already available from plain MUL.
		int32_t a = (int32_t)rs1_val, b = (int32_t)rs2_val;
		uint32_t ua = (uint32_t)rs1_val, ub = (uint32_t)rs2_val;
		uint32_t result32 = 0;
		switch (instr.funct3) {
		case 0b000: result32 = (uint32_t)(a * b); break; // MUL
		case 0b001: result32 = (uint32_t)(((int64_t)a * (int64_t)b) >> 32); break; // MULH
		case 0b010: result32 = (uint32_t)(((int64_t)a * (int64_t)ub) >> 32); break; // MULHSU
		case 0b011: result32 = (uint32_t)(((uint64_t)ua * (uint64_t)ub) >> 32); break; // MULHU
		case 0b100: // DIV
			if (b == 0) result32 = 0xFFFFFFFF;
			else if (a == INT32_MIN && b == -1) result32 = (uint32_t)INT32_MIN;
			else result32 = (uint32_t)(a / b);
			break;
		case 0b101: // DIVU
			result32 = (ub == 0) ? 0xFFFFFFFF : (ua / ub);
			break;
		case 0b110: // REM
			if (b == 0) result32 = (uint32_t)a;
			else if (a == INT32_MIN && b == -1) result32 = 0;
			else result32 = (uint32_t)(a % b);
			break;
		case 0b111: // REMU
			result32 = (ub == 0) ? ua : (ua % ub);
			break;
		}
		result = sext32(result32);
	} else {
		switch (instr.funct3) {
		case 0b000: // MUL -- low 64 bits of the product, same regardless of signedness
			result = rs1_val * rs2_val;
			break;
		case 0b001: { // MULH (signed x signed, upper 64 bits of a 128-bit product)
			__int128 prod = (__int128)rs1_s * (__int128)rs2_s;
			result = (uint64_t)((unsigned __int128)prod >> 64);
			break;
		}
		case 0b010: { // MULHSU (signed x unsigned)
			__int128 prod = (__int128)rs1_s * (__int128)(unsigned __int128)rs2_val;
			result = (uint64_t)((unsigned __int128)prod >> 64);
			break;
		}
		case 0b011: { // MULHU (unsigned x unsigned)
			unsigned __int128 prod = (unsigned __int128)rs1_val * (unsigned __int128)rs2_val;
			result = (uint64_t)(prod >> 64);
			break;
		}
		case 0b100: // DIV
			if (rs2_s == 0) result = UINT64_MAX;
			else if (rs1_s == INT64_MIN && rs2_s == -1) result = (uint64_t)INT64_MIN;
			else result = (uint64_t)(rs1_s / rs2_s);
			break;
		case 0b101: // DIVU
			result = (rs2_val == 0) ? UINT64_MAX : (rs1_val / rs2_val);
			break;
		case 0b110: // REM
			if (rs2_s == 0) result = rs1_val;
			else if (rs1_s == INT64_MIN && rs2_s == -1) result = 0;
			else result = (uint64_t)(rs1_s % rs2_s);
			break;
		case 0b111: // REMU
			result = (rs2_val == 0) ? rs1_val : (rs1_val % rs2_val);
			break;
		}
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}
