#pragma once
#include "riscv_decoder.hpp"
#include <cstdint>

class Registers;
class Memory;

class RiscvCore {
public:
	RiscvCore();

	// Each function is responsible for leaving pc correctly set before
	// returning -- advanced by the instruction's length for straight-line
	// code, or set to the branch/jump target. No separate auto-increment
	// step exists elsewhere.
	void exec_32I(const DecodedInstruction &instr, Registers &regs, Memory &mem);
	void exec_32M(const DecodedInstruction &instr, Registers &regs, Memory &mem);
	void exec_32A(const DecodedInstruction &instr, Registers &regs, Memory &mem);

private:
	// LR.W/SC.W reservation state. Single-hart, no interrupts, so this only
	// ever needs to survive the immediate LR->SC pair a retry loop does --
	// no cross-hart invalidation logic needed.
	bool reservation_valid;
	uint32_t reservation_addr;
};
