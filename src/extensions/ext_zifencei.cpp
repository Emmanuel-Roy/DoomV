// Zifencei: FENCE.I, instruction-fetch synchronisation.
//
// Implemented as a genuine no-op rather than left unsupported, and the
// distinction matters. FENCE.I means "make sure instruction fetches see
// stores that have already happened". There is no instruction cache here --
// every fetch reads straight out of live guest memory via
// DoomSystem::step() -- so that guarantee already holds unconditionally, and
// the correct emulation is to do nothing at all. The decoder's own cache is
// keyed on (address, encoding), so self-modifying code misses and re-decodes
// on its own without needing to be told.
//
// Shares opcode 0001111 with base I's FENCE, split by funct3 in
// Decoder::classify(): funct3=001 is FENCE.I, anything else is FENCE.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zifencei(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZIFENCEI;
	instr.length = 4;
	instr.mnemonic = "FENCE.I";

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;

	return instr;
}

void RiscvCore::exec_ZIFENCEI(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	// Nothing to synchronise -- see the file header.
	regs.set_pc(regs.get_pc() + instr.length);
}
