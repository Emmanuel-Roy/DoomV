// Zihintpause: the PAUSE hint.
//
// Encoded as FENCE with pred=W, succ=none (0x0100000F), which means it sits
// in base I's MISC-MEM space and already retired harmlessly as a plain
// FENCE before this file existed. Splitting it out buys two things: the
// dashboard names it correctly, and -march can gate it, so a guest built
// without Zihintpause cannot silently rely on it being accepted.
//
// A hint carries no architectural effect, so there is nothing to do but
// advance pc. On a single-hart interpreter there is not even a spin loop to
// yield to.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zihintpause(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZIHINTPAUSE;
	instr.length = 4;
	instr.mnemonic = "PAUSE";
	instr.opcode = raw_instr & 0x7F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	return instr;
}

void RiscvCore::exec_ZIHINTPAUSE(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	regs.set_pc(regs.get_pc() + instr.length);
}
