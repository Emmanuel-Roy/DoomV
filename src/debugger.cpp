#include "debugger.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

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

void Debugger::add_breakpoint(uint64_t addr)
{
	breakpoints.push_back(addr);
}

void Debugger::dump_signature(Memory &mem, uint64_t begin, uint64_t end, const char *path)
{
	std::ofstream file(path);
	if (!file.is_open()) return;

	for (uint64_t addr = begin; addr < end; addr += 4) {
		file << std::hex << std::setw(8) << std::setfill('0') << mem.read32(addr) << "\n";
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

	auto hex = [&](uint64_t v, int width) -> std::ostream & {
		return file << std::hex << std::setw(width) << std::setfill('0') << v;
	};

	file << "pc "; hex(regs.get_pc(), 16) << "\n";
	for (int i = 0; i < 32; i++) {
		file << "x" << std::dec << i << " "; hex(regs.read_x(i), 16) << "\n";
	}
	for (int i = 0; i < 32; i++) {
		uint64_t bits;
		double d = regs.read_f(i);
		std::memcpy(&bits, &d, 8);
		file << "f" << std::dec << i << " "; hex(bits, 16) << "\n";
	}
	for (int i = 0; i < 32; i++) {
		const uint8_t *v = regs.read_v(i);
		file << "v" << std::dec << i << " ";
		for (int b = Registers::VLEN_BYTES - 1; b >= 0; b--) {
			file << std::hex << std::setw(2) << std::setfill('0') << (int)v[b];
		}
		file << "\n";
	}
	file << "vtype "; hex(regs.get_vtype(), 16) << "\n";
	file << "vl "; hex(regs.get_vl(), 16) << "\n";
	file << "vstart "; hex(regs.get_vstart(), 16) << "\n";
	file << "vxrm "; hex(regs.get_vxrm(), 2) << "\n";
	file << "vxsat "; hex(regs.get_vxsat(), 2) << "\n";
	file << "fflags "; hex(regs.get_fflags(), 2) << "\n";
	file << "frm "; hex(regs.get_frm(), 2) << "\n";
	file << "---trace---\n";

	int pos = regs.history_pos();
	for (int i = 0; i < Registers::HISTORY_SIZE; i++) {
		const HistoryEntry &h = regs.history_at((pos + i) % Registers::HISTORY_SIZE);
		file << std::hex << std::setw(16) << std::setfill('0') << h.pc
		     << ": " << std::setw(8) << std::setfill('0') << h.instr << "\n";
	}
}
