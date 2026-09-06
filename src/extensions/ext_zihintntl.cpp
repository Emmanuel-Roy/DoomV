// Zihintntl: non-temporal locality hints.
//
// The four hints (ntl.p1, ntl.pall, ntl.s1, ntl.all) are encoded as
// C.ADD x0, rs2 with rs2 in 2..5 -- so they were already being decoded as a
// compressed add whose destination is x0, which discards the result. In
// other words they have always retired correctly here, by accident rather
// than intent.
//
// They tell a cache which locality to expect for the *following* memory
// access. There is no cache, so there is nothing to hint at; what this file
// adds is that the hint decodes under its own name and gates on its own
// extension rather than masquerading as arithmetic.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zihintntl(uint16_t raw16) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZIHINTNTL;
	instr.length = 2;

	switch ((raw16 >> 2) & 0x1F) { // the rs2 field selects which hint
	case 2:  instr.mnemonic = "C.NTL.P1";   break;
	case 3:  instr.mnemonic = "C.NTL.PALL"; break;
	case 4:  instr.mnemonic = "C.NTL.S1";   break;
	default: instr.mnemonic = "C.NTL.ALL";  break;
	}
	return instr;
}

void RiscvCore::exec_ZIHINTNTL(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	regs.set_pc(regs.get_pc() + instr.length);
}
