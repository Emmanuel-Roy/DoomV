#pragma once
#include "riscv_decoder.hpp"

class Registers;
class Memory;

class RiscvCore {
public:
	// Each function is responsible for leaving pc correctly set before
	// returning -- advanced by the instruction's length for straight-line
	// code, or set to the branch/jump target. No separate auto-increment
	// step exists elsewhere. This is the CPU core logic -- yours to write.
	void exec_32I(const DecodedInstruction &instr, Registers &regs, Memory &mem);
	void exec_32M(const DecodedInstruction &instr, Registers &regs, Memory &mem);
	void exec_32A(const DecodedInstruction &instr, Registers &regs, Memory &mem);
};
