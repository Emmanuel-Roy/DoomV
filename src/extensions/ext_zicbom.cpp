// Zicbom: cache-block management.
//
// cbo.inval, cbo.clean and cbo.flush act on the cache block containing the
// address in rs1. DoomV has no cache -- every load and store goes straight
// to Memory -- so the coherence these instructions buy is already
// unconditionally true here, and all three retire without effect.
//
// That is a real implementation, not a stub: an emulator with no cache
// hierarchy satisfies the architectural contract trivially. What it does
// not model is the *permission* layer -- menvcfg.CBIE/CBCFE and
// senvcfg.CBIE let M-mode make these trap or degrade in S/U mode, and DoomV
// permits them at every privilege level. Nothing in the boot path
// restricts them, so the difference is unobservable today; it would matter
// to a guest deliberately testing the envcfg gating.
//
// They live in MISC-MEM (the FENCE opcode) at funct3=010, a slot base I
// leaves empty -- so unlike the hint extensions, disabling Zicbom really
// does make these illegal rather than reverting them to some base meaning.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zicbom(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZICBOM;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.imm    = (raw_instr >> 20) & 0xFFF; // selects which of the three

	switch (instr.imm) {
	case 0: instr.mnemonic = "CBO.INVAL"; break;
	case 1: instr.mnemonic = "CBO.CLEAN"; break;
	default: instr.mnemonic = "CBO.FLUSH"; break;
	}
	return instr;
}

void RiscvCore::exec_ZICBOM(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	regs.set_pc(regs.get_pc() + instr.length);
}
