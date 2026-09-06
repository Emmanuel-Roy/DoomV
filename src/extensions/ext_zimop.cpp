// Zimop: may-be-operations.
//
// Thirty-two MOP.R.n and eight MOP.RR.n encodings that are architecturally
// defined to write zero to rd and do nothing else. The point is forward
// compatibility: they are reserved encodings with *defined* behaviour, so a
// future extension can claim one and older software still runs -- it sees a
// zero rather than an illegal instruction.
//
// That makes "write zero" the whole specification, and getting it wrong in
// the obvious way (leaving rd untouched) would be invisible until some
// guest depended on the zero.
//
// They live in SYSTEM funct3=100, a slot Zicsr leaves unused. Bit 31 marks
// the space and bit 25 picks the form: clear for MOP.R (one source), set
// for MOP.RR (two). The n index is scattered across the remaining bits and
// carries no meaning here, since every n behaves identically.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zimop(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZIMOP;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	instr.mnemonic = ((raw_instr >> 25) & 1) ? "MOP.RR" : "MOP.R";
	return instr;
}

void RiscvCore::exec_ZIMOP(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	regs.write_x(instr.rd, 0);
	regs.set_pc(regs.get_pc() + instr.length);
}
