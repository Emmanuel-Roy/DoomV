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
	const char *mnemonic; // e.g. "ADDI" -- static string, name only, no operands
};

class Registers;
class Memory;
class RiscvCore;

struct DispatchResult {
	bool illegal;
	const char *mnemonic;
};

class Decoder {
public:
	Decoder(RiscvCore &core, Registers &regs, Memory &mem);

	DispatchResult decode_and_dispatch(uint32_t raw_instr);

private:
	RiscvCore &core;
	Registers &regs;
	Memory &mem;

	// classify()/decode() figure out *what* an instruction is (opcode
	// tables, field extraction, mnemonic for display) -- exec_32I/M/A in
	// RiscvCore are what actually *do* something with it, and stay stubs.
	Extension classify(uint32_t raw_instr) const;
	DecodedInstruction decode(uint32_t raw_instr, Extension ext) const;
};
