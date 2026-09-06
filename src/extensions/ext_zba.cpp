// Zba: address-generation. Shifted adds (sh1add/sh2add/sh3add), their
// zero-extending .uw forms, add.uw and slli.uw -- the handful of operations
// that exist so array indexing does not need a separate shift.
//
// Shares OP/OP-32/OP-IMM-32 with base I, M and the other bitmanip families,
// split by funct7 (or funct6 where a 6-bit shamt eats funct7's low bit) in
// Decoder::classify(). Fields are re-extracted here rather than shared from
// a prelude, matching what every other decode_* in this project does.
//
// The .uw forms zero-extend rs1's low half *before* the shift, which is the
// whole point of having them: on RV64 an unsigned 32-bit array index would
// otherwise need an explicit mask first.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zba(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZBA;
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
	instr.imm = (raw_instr >> 20) & 0x3F; // slli.uw's 6-bit shamt

	uint8_t funct3 = instr.funct3;
	uint8_t funct7 = instr.funct7;

	switch (opcode) {
	case 0b0110011: // OP -- sh{1,2,3}add
		instr.mnemonic = (funct3 == 0b010) ? "SH1ADD" : (funct3 == 0b100) ? "SH2ADD" : "SH3ADD";
		break;
	case 0b0111011: // OP-32 -- add.uw and the .uw shifted adds
		if (funct7 == 0b0000100) {
			instr.mnemonic = "ADD.UW";
		} else {
			instr.mnemonic = (funct3 == 0b010) ? "SH1ADD.UW" : (funct3 == 0b100) ? "SH2ADD.UW" : "SH3ADD.UW";
		}
		break;
	case 0b0011011: // OP-IMM-32 -- slli.uw
		instr.mnemonic = "SLLI.UW";
		break;
	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_ZBA(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();
	uint64_t rs1_val = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);
	uint64_t result = 0;

	uint8_t funct3 = instr.funct3;
	uint8_t funct7 = instr.funct7;

	switch (instr.opcode) {
	case 0b0110011: { // sh{1,2,3}add
		unsigned sh = (funct3 == 0b010) ? 1u : (funct3 == 0b100) ? 2u : 3u;
		result = (rs1_val << sh) + rs2_val;
		break;
	}
	case 0b0111011:
		if (funct7 == 0b0000100) { // add.uw
			result = (uint64_t)(uint32_t)rs1_val + rs2_val;
		} else { // sh{1,2,3}add.uw
			unsigned sh = (funct3 == 0b010) ? 1u : (funct3 == 0b100) ? 2u : 3u;
			result = ((uint64_t)(uint32_t)rs1_val << sh) + rs2_val;
		}
		break;
	case 0b0011011: // slli.uw -- zero-extend first, then shift in full 64 bits
		result = (uint64_t)(uint32_t)rs1_val << ((uint64_t)instr.imm & 63);
		break;
	default:
		break;
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}
