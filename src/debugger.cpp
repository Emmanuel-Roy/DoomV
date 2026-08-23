#include "debugger.hpp"
#include "registers.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

Debugger::Debugger() : halted(false)
{
}

void Debugger::load_breakpoints(const char *path)
{
	std::ifstream file(path);
	if (!file.is_open()) return;

	std::string line;
	while (std::getline(file, line)) {
		if (line.empty()) continue;
		breakpoints.push_back((uint64_t)std::stoull(line, nullptr, 16));
	}
}

bool Debugger::should_halt(uint64_t pc, bool instr_was_illegal)
{
	if (instr_was_illegal) {
		halted = true;
		return true;
	}

	if (std::find(breakpoints.begin(), breakpoints.end(), pc) != breakpoints.end()) {
		halted = true;
		return true;
	}

	return false;
}

void Debugger::dump_log(const Registers &regs, const char *path)
{
	std::ofstream file(path);
	if (!file.is_open()) return;

	int pos = regs.history_pos();
	for (int i = 0; i < Registers::HISTORY_SIZE; i++) {
		const HistoryEntry &h = regs.history_at((pos + i) % Registers::HISTORY_SIZE);
		file << std::hex << std::setw(16) << std::setfill('0') << h.pc
		     << ": " << std::setw(8) << std::setfill('0') << h.instr << "\n";
	}
}
