// Zicbop: cache-block prefetch hints.
//
// prefetch.i, prefetch.r and prefetch.w say a cache block is about to be
// fetched, read or written. They are hints in the strictest sense: an
// implementation is free to ignore them entirely, and one with no cache
// has nothing else it could do.
//
// Their encoding is ORI with rd=x0 and the low five bits of the immediate
// selecting the variant -- an encoding base I already defines as a reserved
// HINT, and which DoomV was therefore already retiring correctly, since a
// write to x0 is discarded. So this file changes no behaviour; it makes the
// instruction decode under its own name instead of appearing as an ORI
// whose result vanishes.
//
// Because the base ISA owns this encoding too, classify() only routes here
// when Zicbop is enabled. Turning the extension off has to leave a plain
// ORI behind rather than an illegal instruction -- the encoding stays
// legal either way, it just stops being called a prefetch.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zicbop(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZICBOP;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	// imm[11:5] is the (block-aligned) offset; imm[4:0] picks the variant.
	instr.imm    = (int64_t)(((int32_t)raw_instr >> 25) << 5);

	switch ((raw_instr >> 20) & 0x1F) {
	case 0: instr.mnemonic = "PREFETCH.I"; break;
	case 1: instr.mnemonic = "PREFETCH.R"; break;
	default: instr.mnemonic = "PREFETCH.W"; break;
	}
	return instr;
}

void RiscvCore::exec_ZICBOP(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	regs.set_pc(regs.get_pc() + instr.length);
}
