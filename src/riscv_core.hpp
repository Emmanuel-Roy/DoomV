#pragma once
#include "riscv_decoder.hpp"
#include "mmu.hpp"
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

	// Sv39 address translation, shared by every load/store/AMO/FP-load/
	// vector-load call site and by DoomSystem::step()'s instruction fetch.
	// On a page fault this enters a trap itself (same as enter_trap below)
	// and returns false -- the caller should just abort the instruction
	// without touching Memory, since pc has already been redirected.
	bool translate_or_trap(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type, uint64_t &paddr);

	// Called once per DoomSystem::step(), before fetch. Computes the
	// effective mip & mie, picks the highest-priority pending+enabled+
	// unmasked interrupt (if any) per the spec's fixed priority order,
	// and -- if one is actually deliverable at the current privilege/
	// mstatus.xIE state -- takes it via enter_trap. Returns true if an
	// interrupt was taken (the caller should skip the rest of this step,
	// same shape translate_or_trap already established for page faults).
	bool check_and_take_interrupt(Registers &regs, Memory &mem);

	// The same "what does a CSR read actually return" logic
	// exec_32ZICSR's read side uses (ext_zicsr.cpp) -- several CSRs are
	// computed, not plain csr[] storage (sstatus, mip, misa, time, the
	// IMSIC-backed indirect/claim registers, ...), so a caller that just
	// wants to *peek* a live value (the dashboard's CSRs panel) needs
	// this instead of Registers::read_csr directly, or it'd see stale/
	// wrong values for exactly the CSRs most worth watching. Side-effect
	// free -- topei_value() (used here) is a plain peek; claim() is a
	// separate call exec_32ZICSR only makes on an actual write.
	uint64_t read_csr_effective(Registers &regs, Memory &mem, uint16_t csr);

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
	void enter_trap(Registers &regs, uint64_t cause, uint64_t tval, bool is_interrupt = false);
};
