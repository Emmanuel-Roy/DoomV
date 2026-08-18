#pragma once
#include <cstdint>
#include <vector>

class Registers;

class Debugger {
public:
	Debugger();

	void load_breakpoints(const char *path);
	bool should_halt(uint32_t pc, bool instr_was_illegal);

	void dump_log(const Registers &regs, const char *path);

	bool halted;

private:
	std::vector<uint32_t> breakpoints;
};
