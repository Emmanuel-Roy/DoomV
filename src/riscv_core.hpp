#pragma once
#include "riscv_decoder.hpp"
#include <cstdint>

class Registers;
class Memory;

// sign-extend the low 32 bits of a 64-bit value to the full 64 -- the
// operation every *W-suffixed RV64 instruction ends with. Shared by
// ext_i.cpp and ext_m.cpp.
inline uint64_t sext32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }

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
	void exec_32ZICSR(const DecodedInstruction &instr, Registers &regs, Memory &mem);
	void exec_FD(const DecodedInstruction &instr, Registers &regs, Memory &mem);
	void exec_V(const DecodedInstruction &instr, Registers &regs, Memory &mem);

private:
	// LR/SC reservation state. Single-hart, no interrupts, so this only
	// ever needs to survive the immediate LR->SC pair a retry loop does --
	// no cross-hart invalidation logic needed. Doesn't separately track
	// LR.W vs LR.D width (a mixed-width LR->SC pair at the same address
	// would incorrectly succeed) -- not something compiled code produces.
	bool reservation_valid;
	uint64_t reservation_addr;

	// ECALL/EBREAK both funnel into the same M-mode trap entry sequence
	// (illegal-instruction detection stays a separate debugger safety net,
	// see exec_32ZICSR's comment -- this is only for the deliberate,
	// explicit trap-requesting instructions).
	void enter_trap(Registers &regs, uint64_t cause, uint64_t tval);
};
