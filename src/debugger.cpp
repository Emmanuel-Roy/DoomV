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

void Debugger::dump_log(const Registers &regs, Memory &mem, const char *path)
{
	std::ofstream file(path);
	if (!file.is_open()) return;

	auto hex = [&](uint64_t v, int width) -> std::ostream & {
		return file << std::hex << std::setw(width) << std::setfill('0') << v;
	};

	file << "pc "; hex(regs.get_pc(), 16) << "\n";
	file << "priv " << (int)regs.get_priv() << "\n"; // 0=U, 1=S, 3=M -- see PrivMode
	// A handful of privileged CSRs relevant to trap/paging debugging --
	// named by raw address (their symbolic constants are private to
	// ext_zicsr.cpp) rather than the full generic csr[] array, which is
	// mostly zero/irrelevant noise for this dump's purpose.
	file << "mstatus "; hex(regs.read_csr(0x300), 16) << "\n";
	file << "medeleg "; hex(regs.read_csr(0x302), 16) << "\n";
	file << "mtvec ";   hex(regs.read_csr(0x305), 16) << "\n";
	file << "mepc ";    hex(regs.read_csr(0x341), 16) << "\n";
	file << "mcause ";  hex(regs.read_csr(0x342), 16) << "\n";
	file << "mtval ";   hex(regs.read_csr(0x343), 16) << "\n";
	file << "stvec ";   hex(regs.read_csr(0x105), 16) << "\n";
	file << "sepc ";    hex(regs.read_csr(0x141), 16) << "\n";
	file << "scause ";  hex(regs.read_csr(0x142), 16) << "\n";
	file << "stval ";   hex(regs.read_csr(0x143), 16) << "\n";
	file << "satp ";    hex(regs.read_csr(0x180), 16) << "\n";
	// Stage 2 (timer + AIA): mip/sie are shown raw (the software-writable
	// shadow this project stores, not the computed effective value
	// ext_zicsr.cpp's compute_mip/read_sie produce -- that logic is
	// private to that file) -- still useful alongside mie/mideleg/
	// menvcfg/stimecmp and the timer's own state for confirming what a
	// hand-written interrupt test actually did.
	file << "mie ";      hex(regs.read_csr(0x304), 16) << "\n";
	file << "mip_raw ";  hex(regs.read_csr(0x344), 16) << "\n";
	file << "mideleg ";  hex(regs.read_csr(0x303), 16) << "\n";
	file << "menvcfg ";  hex(regs.read_csr(0x30A), 16) << "\n";
	file << "stimecmp "; hex(regs.read_csr(0x14D), 16) << "\n";
	file << "mtime ";    hex(mem.get_timer().get_mtime(), 16) << "\n";
	file << "mtimecmp "; hex(mem.get_timer().get_mtimecmp(), 16) << "\n";
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
