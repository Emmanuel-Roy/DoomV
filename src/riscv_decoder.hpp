#pragma once
#include <cstdint>

enum class Extension {
	I,
	M,
	A,
	C,
	ZICSR,
	V,
	ILLEGAL,
};

struct DecodedInstruction {
	Extension ext;
	uint8_t opcode;
	uint8_t rd, rs1, rs2;
	uint8_t funct3, funct7;
	int32_t imm;
	uint8_t length; // 2 or 4 bytes
};

class Registers;
class Memory;
class RiscvCore;

struct DispatchResult {
	bool illegal;
};

class Decoder {
public:
	Decoder(RiscvCore &core, Registers &regs, Memory &mem);

	DispatchResult decode_and_dispatch(uint32_t raw_instr);

private:
	RiscvCore &core;
	Registers &regs;
	Memory &mem;

	// TODO: classify raw_instr by opcode/funct7 into an Extension tag
	// (pure classification data, see PLAN.md), then decode its fields
	// into a DecodedInstruction. This is the CPU core logic -- yours to
	// write.
	Extension classify(uint32_t raw_instr) const;
	DecodedInstruction decode(uint32_t raw_instr, Extension ext) const;
};
