// Zbs: single-bit operations. bclr/bext/binv/bset, each in a register form
// (bit index from rs2) and an immediate form (bit index from a 6-bit shamt).
//
// Shares OP/OP-IMM with base I and the other bitmanip families, split by
// funct7 (or funct6 for the immediate forms, where the 6-bit shamt claims
// funct7's low bit) in Decoder::classify().
//
// Bit indices are taken modulo XLEN in both forms -- an index of 65 selects
// bit 1, not "past the end". That is spec behaviour, not a convenience.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zbs(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZBS;
	instr.length = 4;
	instr.mnemonic = "???";

	uint8_t opcode = raw_instr & 0x7F;
	instr.opcode = opcode;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;
	instr.imm = (raw_instr >> 20) & 0x3F; // immediate-form bit index

	uint8_t funct6 = (raw_instr >> 26) & 0x3F;
	uint8_t funct3 = instr.funct3;
	uint8_t funct7 = instr.funct7;

	if (opcode == 0b0110011) { // register forms
		if (funct7 == 0b0100100)      instr.mnemonic = (funct3 == 0b001) ? "BCLR" : "BEXT";
		else if (funct7 == 0b0110100) instr.mnemonic = "BINV";
		else                          instr.mnemonic = "BSET";
	} else { // OP-IMM, immediate forms
		if (funct3 == 0b001) {
			instr.mnemonic = (funct6 == 0b001010) ? "BSETI" : (funct6 == 0b010010) ? "BCLRI" : "BINVI";
		} else {
			instr.mnemonic = "BEXTI";
		}
	}

	return instr;
}

void RiscvCore::exec_ZBS(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();
	uint64_t rs1_val = regs.read_x(instr.rs1);
	uint64_t result = 0;

	uint8_t funct3 = instr.funct3;
	uint8_t funct7 = instr.funct7;
	uint8_t funct6 = (uint8_t)(funct7 >> 1);

	if (instr.opcode == 0b0110011) { // register forms: index from rs2
		unsigned n = (unsigned)(regs.read_x(instr.rs2) & 63);
		if (funct7 == 0b0100100) {
			result = (funct3 == 0b001) ? (rs1_val & ~(1ull << n))   // bclr
			                           : ((rs1_val >> n) & 1);      // bext
		} else if (funct7 == 0b0110100) {
			result = rs1_val ^ (1ull << n);                          // binv
		} else {
			result = rs1_val | (1ull << n);                          // bset
		}
	} else { // OP-IMM: index from the shamt field
		unsigned n = (unsigned)((uint64_t)instr.imm & 63);
		if (funct3 == 0b001) {
			result = (funct6 == 0b001010) ? (rs1_val | (1ull << n))   // bseti
			       : (funct6 == 0b010010) ? (rs1_val & ~(1ull << n))  // bclri
			                              : (rs1_val ^ (1ull << n));  // binvi
		} else {
			result = (rs1_val >> n) & 1;                              // bexti
		}
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}
