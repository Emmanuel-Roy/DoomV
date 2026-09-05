#pragma once
#include <cstdint>
#include <vector>

class Registers;
class Memory;

class Debugger {
public:
	Debugger();

	void load_breakpoints(const char *path);
	void add_breakpoint(uint64_t addr);
	bool should_halt(uint64_t pc, bool instr_was_illegal);

	void dump_log(const Registers &regs, Memory &mem, const char *path);

	// Test/verification hook: dumps [begin, end) as one 32-bit hex word per
	// line, matching the riscv-arch-test convention closely enough to diff
	// against a reference simulator's own signature-region dump -- used by
	// the F/D arch-test harness, not by normal Doom runs.
	void dump_signature(Memory &mem, uint64_t begin, uint64_t end, const char *path);

	bool halted;

	// Clears `halted` and arms a one-shot skip for `pc`. Without the skip a
	// breakpoint could never be resumed past: pc still sits on it, so the
	// very next should_halt() would match again and re-freeze immediately.
	void resume(uint64_t pc);

private:
	std::vector<uint64_t> breakpoints;
	uint64_t skip_bp_pc = 0;
	bool skip_bp_armed = false;
};
