// Zcmop: compressed may-be-operations.
//
// The compressed counterpart to Zimop: eight C.MOP.n encodings, n odd from
// 1 to 15. Unlike MOP.R/MOP.RR these name no destination register at all,
// so they leave every register unchanged -- the reserved-but-defined
// behaviour is simply "nothing happened".
//
// They occupy quadrant 1, funct3=011 with bits[6:2] zero and bit 7 set,
// which is the C.LUI encoding whose immediate would be zero. That form is
// reserved in base RVC precisely so Zcmop could claim it later.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zcmop(uint16_t raw16) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZCMOP;
	instr.length = 2;
	instr.mnemonic = "C.MOP";
	(void)raw16; // n is encoded in bits[11:8] but every n behaves the same
	return instr;
}

void RiscvCore::exec_ZCMOP(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	regs.set_pc(regs.get_pc() + instr.length);
}
