#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"

Decoder::Decoder(RiscvCore &core, Registers &regs, Memory &mem)
	: core(core), regs(regs), mem(mem)
{
}

Extension Decoder::classify(uint32_t raw_instr) const
{
	(void)raw_instr;
	// TODO: opcode/funct7 -> Extension classification table
	return Extension::ILLEGAL;
}

DecodedInstruction Decoder::decode(uint32_t raw_instr, Extension ext) const
{
	(void)raw_instr;
	// TODO: extract rd/rs1/rs2/imm/funct3/funct7/length
	DecodedInstruction instr{};
	instr.ext = ext;
	return instr;
}

DispatchResult Decoder::decode_and_dispatch(uint32_t raw_instr)
{
	Extension ext = classify(raw_instr);

	bool enabled = (ext == Extension::I && Extensions::I)
	            || (ext == Extension::M && Extensions::M)
	            || (ext == Extension::A && Extensions::A)
	            || (ext == Extension::C && Extensions::C)
	            || (ext == Extension::ZICSR && Extensions::ZICSR)
	            || (ext == Extension::V && Extensions::V);

	if (!enabled) {
		return {true};
	}

	DecodedInstruction instr = decode(raw_instr, ext);

	switch (ext) {
	case Extension::I:
		core.exec_32I(instr, regs, mem);
		break;
	case Extension::M:
		core.exec_32M(instr, regs, mem);
		break;
	case Extension::A:
		core.exec_32A(instr, regs, mem);
		break;
	default:
		return {true};
	}

	return {false};
}
