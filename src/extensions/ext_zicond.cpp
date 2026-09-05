// Zicond: integer conditional operations. Two instructions, czero.eqz and
// czero.nez, which zero the destination based on rs2 and otherwise pass rs1
// through -- the primitive a compiler uses to build a branchless select.
//
// Shares OP with base I, M and the bitmanip families, split out on
// funct7=0000111 in Decoder::classify().
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zicond(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZICOND;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	instr.mnemonic = (instr.funct3 == 0b101) ? "CZERO.EQZ" : "CZERO.NEZ";
	return instr;
}

void RiscvCore::exec_ZICOND(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();
	uint64_t rs1_val = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);

	// czero.eqz: rd = (rs2 == 0) ? 0 : rs1   -- "zero if the condition is zero"
	// czero.nez: rd = (rs2 != 0) ? 0 : rs1
	uint64_t result = (instr.funct3 == 0b101) ? ((rs2_val == 0) ? 0 : rs1_val)
	                                          : ((rs2_val != 0) ? 0 : rs1_val);

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}
