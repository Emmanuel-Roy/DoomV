// The machine half of -gdb=<port>: gdb's remote serial protocol answered on
// the CPU thread, between instructions.
//
//   riscv_doom.exe ... -gdb=1234            (waits, halted, for gdb)
//   riscv64-unknown-elf-gdb prog.elf -ex "target remote :1234"
//
// -gdb=<address>:<port> listens on another of this computer's addresses
// instead of 127.0.0.1 -- the one WSL reaches Windows by, for a gdb in WSL.
//
// What gdb gets:
//   * The registers by gdb's RISC-V numbering: x0-x31, pc, f0-f31, every CSR
//     at 65 + its number, the privilege level at 4161 and v0-v31 at 4162 on,
//     described to gdb by a target description so it knows the vector width.
//     x, f, v and pc can be written; CSRs are read-only, because writing one
//     has effects (translation, interrupts) that the hart's own CSR
//     instructions take care of and a debugger's write would not.
//   * Memory, by the address gdb asks for as the hart sees it now: virtual,
//     through satp (or vsatp, for a guest with an untranslated G-stage),
//     unless the hart is in M-mode. The walk here is its own, read-only, so
//     looking does not set a page's A or D bit; and only RAM is reached --
//     device registers can change when read, so a debugger read of one could
//     change the guest.
//   * Breakpoints (Z0/Z1), single steps, continue, and Ctrl-C to stop.
//     Several harts are gdb's threads 1 to N; a step is one round of them.
//   * monitor steps, monitor snapshot <dir>.
//
// Stopping, stepping and looking change nothing the guest can see: inputs are
// still committed at instruction counts, so a run under gdb is the same run
// as one without it, as long as gdb writes nothing.
#include "doom_system.hpp"
#include "mmu.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {

const char HEX[] = "0123456789abcdef";

std::string to_hex(const uint8_t *p, size_t n)
{
	std::string s;
	s.reserve(n * 2);
	for (size_t i = 0; i < n; i++) {
		s.push_back(HEX[p[i] >> 4]);
		s.push_back(HEX[p[i] & 0xF]);
	}
	return s;
}

std::string le_hex(uint64_t v, size_t bytes)
{
	uint8_t b[8];
	for (size_t i = 0; i < bytes; i++) b[i] = (uint8_t)(v >> (8 * i));
	return to_hex(b, bytes);
}

bool from_hex(const std::string &s, std::vector<uint8_t> &out)
{
	if (s.size() % 2) return false;
	out.clear();
	for (size_t i = 0; i < s.size(); i += 2) {
		char *end = nullptr;
		const std::string two = s.substr(i, 2);
		const unsigned long v = std::strtoul(two.c_str(), &end, 16);
		if (*end) return false;
		out.push_back((uint8_t)v);
	}
	return true;
}

uint64_t le_value(const std::vector<uint8_t> &b)
{
	uint64_t v = 0;
	for (size_t i = 0; i < b.size() && i < 8; i++) v |= (uint64_t)b[i] << (8 * i);
	return v;
}

std::string text_hex(const std::string &t) { return to_hex((const uint8_t *)t.data(), t.size()); }

const char XFER_TARGET[] = "qXfer:features:read:target.xml:";

constexpr unsigned REG_PC = 32, REG_F0 = 33, REG_CSR0 = 65, REG_PRIV = 4161, REG_V0 = 4162;

// The CSRs the target description names, so gdb's `info registers` has
// them; any other CSR is still there to read by number ($csr<n> in gdb).
const struct { const char *name; uint16_t num; } CSRS[] = {
	{"sstatus", 0x100}, {"sie", 0x104}, {"stvec", 0x105}, {"scounteren", 0x106}, {"senvcfg", 0x10A},
	{"sscratch", 0x140}, {"sepc", 0x141}, {"scause", 0x142}, {"stval", 0x143}, {"sip", 0x144},
	{"satp", 0x180}, {"vsstatus", 0x200}, {"vsatp", 0x280}, {"mstatus", 0x300}, {"misa", 0x301},
	{"medeleg", 0x302}, {"mideleg", 0x303}, {"mie", 0x304}, {"mtvec", 0x305}, {"mcounteren", 0x306},
	{"menvcfg", 0x30A}, {"mscratch", 0x340}, {"mepc", 0x341}, {"mcause", 0x342}, {"mtval", 0x343},
	{"mip", 0x344}, {"hstatus", 0x600}, {"hedeleg", 0x602}, {"hideleg", 0x603}, {"hgatp", 0x680},
	{"cycle", 0xC00}, {"time", 0xC01}, {"instret", 0xC02}, {"mhartid", 0xF14},
	{"vstart", 0x008}, {"vxsat", 0x009}, {"vxrm", 0x00A}, {"vcsr", 0x00F},
	{"vl", 0xC20}, {"vtype", 0xC21}, {"vlenb", 0xC22},
};

const char *const XNAMES[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "fp", "s1", "a0", "a1", "a2",
                                "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9",
                                "s10", "s11", "t3", "t4", "t5", "t6"};
const char *const FNAMES[32] = {"ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0",
                                "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5",
                                "fs6", "fs7", "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"};

} // namespace

bool DoomSystem::set_gdb(const std::string &address, int port)
{
	std::string error;
	gdb = new GdbServer();
	if (!gdb->start(address, port, error)) {
		std::cout << "-gdb: " << error << "\n";
		delete gdb;
		gdb = nullptr;
		return false;
	}
	debugger.halted = true;
	std::cout << "gdb: waiting on " << address << ":" << port << " -- in gdb: target remote "
	          << (address == "127.0.0.1" ? "" : address) << ":" << port << std::endl;
	return true;
}

std::string DoomSystem::gdb_stop_reply(int signal)
{
	char buf[48];
	const unsigned thread = (cur ? cur->id : 0) + 1;
	gdb_hart = thread - 1;
	std::snprintf(buf, sizeof buf, "T%02xthread:%x;", signal, thread);
	return buf;
}

void DoomSystem::gdb_service()
{
	if (gdb->take_new_connection()) {
		// gdb opens by asking why the machine stopped: it must be stopped.
		debugger.halted = true;
		gdb_running = false;
	}
	if (!debugger.halted) {
		if (!gdb->take_interrupt()) return;
		debugger.halted = true;   // Ctrl-C, between bursts
		gdb_signal = 2;
	}
	if (gdb_running) {
		gdb_running = false;
		// The program ended (tohost, poweroff, -stopat): gdb is told it exited.
		gdb->send(run_finished ? "W00" : gdb_stop_reply(gdb_signal));
		gdb_signal = 5;
		if (run_finished) gdb_exit_reported = true;
	}
	std::string packet;
	while (debugger.halted && !gdb_running && gdb->next_packet(packet)) gdb_handle(packet);
}

std::string DoomSystem::gdb_target_xml() const
{
	const int vlenb = Registers::VLEN_BYTES;
	std::string x = "<?xml version=\"1.0\"?>\n<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n<target version=\"1.0\">\n"
	                "<architecture>riscv:rv64</architecture>\n<feature name=\"org.gnu.gdb.riscv.cpu\">\n";
	for (unsigned i = 0; i < 32; i++)
		x += std::string("<reg name=\"") + XNAMES[i] + "\" bitsize=\"64\" regnum=\"" + std::to_string(i)
		   + "\" type=\"" + (i == 2 ? "data_ptr" : "int") + "\"/>\n";
	x += "<reg name=\"pc\" bitsize=\"64\" regnum=\"32\" type=\"code_ptr\"/>\n</feature>\n";
	x += "<feature name=\"org.gnu.gdb.riscv.fpu\">\n";
	for (unsigned i = 0; i < 32; i++)
		x += std::string("<reg name=\"") + FNAMES[i] + "\" bitsize=\"64\" regnum=\"" + std::to_string(REG_F0 + i)
		   + "\" type=\"ieee_double\"/>\n";
	x += "<reg name=\"fflags\" bitsize=\"64\" regnum=\"66\" type=\"int\"/>\n"
	     "<reg name=\"frm\" bitsize=\"64\" regnum=\"67\" type=\"int\"/>\n"
	     "<reg name=\"fcsr\" bitsize=\"64\" regnum=\"68\" type=\"int\"/>\n</feature>\n";
	x += "<feature name=\"org.gnu.gdb.riscv.csr\">\n";
	for (const auto &c : CSRS)
		x += std::string("<reg name=\"") + c.name + "\" bitsize=\"64\" regnum=\"" + std::to_string(REG_CSR0 + c.num)
		   + "\" type=\"int\"/>\n";
	x += "</feature>\n<feature name=\"org.gnu.gdb.riscv.virtual\">\n"
	     "<reg name=\"priv\" bitsize=\"64\" regnum=\"4161\" type=\"int\"/>\n</feature>\n";
	if (Extensions.V) {
		x += "<feature name=\"org.gnu.gdb.riscv.vector\">\n";
		x += "<vector id=\"bytes\" type=\"uint8\" count=\"" + std::to_string(vlenb) + "\"/>\n";
		x += "<vector id=\"shorts\" type=\"uint16\" count=\"" + std::to_string(vlenb / 2) + "\"/>\n";
		x += "<vector id=\"words\" type=\"uint32\" count=\"" + std::to_string(vlenb / 4) + "\"/>\n";
		x += "<vector id=\"longs\" type=\"uint64\" count=\"" + std::to_string(vlenb / 8) + "\"/>\n";
		x += "<vector id=\"quads\" type=\"uint128\" count=\"" + std::to_string(vlenb / 16) + "\"/>\n";
		x += "<union id=\"riscv_vector\"><field name=\"b\" type=\"bytes\"/><field name=\"s\" type=\"shorts\"/>"
		     "<field name=\"w\" type=\"words\"/><field name=\"l\" type=\"longs\"/>"
		     "<field name=\"q\" type=\"quads\"/></union>\n";
		for (unsigned i = 0; i < 32; i++)
			x += "<reg name=\"v" + std::to_string(i) + "\" bitsize=\"" + std::to_string(vlenb * 8) + "\" regnum=\""
			   + std::to_string(REG_V0 + i) + "\" type=\"riscv_vector\"/>\n";
		x += "</feature>\n";
	}
	return x + "</target>\n";
}

bool DoomSystem::gdb_read_reg(unsigned n, std::string &hex)
{
	Hart &h = *harts[gdb_hart];
	Registers &r = h.regs;
	if (n < 32) { hex = le_hex(r.read_x((int)n), 8); return true; }
	if (n == REG_PC) { hex = le_hex(r.get_pc(), 8); return true; }
	if (n >= REG_F0 && n < REG_F0 + 32) {
		const double d = r.read_f((int)(n - REG_F0));
		uint64_t bits;
		std::memcpy(&bits, &d, 8);
		hex = le_hex(bits, 8);
		return true;
	}
	if (n >= REG_CSR0 && n < REG_CSR0 + 4096) {
		const uint16_t csr = (uint16_t)(n - REG_CSR0);
		if (csr == 0x015) return false;   // seed: reading it takes entropy from the guest
		hex = le_hex(h.core.read_csr_effective(r, memory, csr), 8);
		return true;
	}
	if (n == REG_PRIV) { hex = le_hex((uint64_t)r.get_priv(), 8); return true; }
	if (n >= REG_V0 && n < REG_V0 + 32 && Extensions.V) {
		hex = to_hex(r.read_v((int)(n - REG_V0)), (size_t)Registers::VLEN_BYTES);
		return true;
	}
	return false;
}

bool DoomSystem::gdb_write_reg(unsigned n, const std::string &hex)
{
	Registers &r = harts[gdb_hart]->regs;
	std::vector<uint8_t> b;
	if (!from_hex(hex, b)) return false;
	if (n < 32 && b.size() == 8) { r.write_x((int)n, le_value(b)); return true; }
	if (n == REG_PC && b.size() == 8) { r.set_pc(le_value(b)); return true; }
	if (n >= REG_F0 && n < REG_F0 + 32 && b.size() == 8) {
		const uint64_t bits = le_value(b);
		double d;
		std::memcpy(&d, &bits, 8);
		r.write_f((int)(n - REG_F0), d);
		return true;
	}
	if (n >= REG_V0 && n < REG_V0 + 32 && Extensions.V && b.size() == (size_t)Registers::VLEN_BYTES) {
		std::memcpy(r.write_v((int)(n - REG_V0)), b.data(), b.size());
		return true;
	}
	return false;
}

// The hart's view of an address, without touching the guest: a page walk of
// its own that sets no A or D bit. RAM holds the page tables; anything else
// ends the walk.
bool DoomSystem::gdb_translate(uint64_t vaddr, uint64_t &paddr)
{
	Hart &h = *harts[gdb_hart];
	Registers &r = h.regs;
	const auto ram64 = [&](uint64_t pa, uint64_t &v) {
		if (!Memory::in_ram(pa, 8)) return false;
		std::memcpy(&v, memory.ram_data() + (pa - Memory::RAM_BASE), 8);
		return true;
	};
	const auto walk = [&](uint64_t satp, uint64_t va, uint64_t &pa) {
		const unsigned mode = (unsigned)(satp >> 60);
		if (mode == 0) { pa = va; return true; }
		if (mode < 8 || mode > 10) return false;
		const int levels = (int)mode - 5;   // Sv39 3, Sv48 4, Sv57 5
		uint64_t table = (satp & ((1ull << 44) - 1)) << 12;
		for (int level = levels - 1; level >= 0; level--) {
			uint64_t pte;
			if (!ram64(table + ((va >> (12 + 9 * level)) & 0x1FF) * 8, pte) || !(pte & 1)) return false;
			uint64_t ppn = (pte >> 10) & ((1ull << 44) - 1);
			if (pte & 0xA) {   // R or X: a leaf
				if ((pte >> 63) && level == 0) ppn = (ppn & ~0xFull) | ((va >> 12) & 0xF);   // Svnapot 64K
				const uint64_t low = (1ull << (12 + 9 * level)) - 1;
				pa = ((ppn << 12) & ~low) | (va & low);
				return true;
			}
			table = ppn << 12;
		}
		return false;
	};
	if (r.get_priv() == PrivMode::M) { paddr = vaddr; return true; }
	if (r.get_virt()) {
		// A guest's address: through vsatp, and only when the G-stage is
		// bare -- the guest-physical address is then the physical one.
		if ((h.core.read_csr_effective(r, memory, 0x680) >> 60) != 0) return false;
		return walk(h.core.read_csr_effective(r, memory, 0x280), vaddr, paddr);
	}
	return walk(h.core.read_csr_effective(r, memory, 0x180), vaddr, paddr);
}

void DoomSystem::gdb_monitor(const std::string &command)
{
	std::string out;
	if (command == "steps") {
		out = "step " + std::to_string(memory.instruction_count()) + "\n";
	} else if (command.rfind("snapshot ", 0) == 0) {
		const std::string dir = command.substr(9);
		out = save_snapshot(dir) ? "saved the machine at step " + std::to_string(memory.instruction_count())
		                               + " to " + dir + "\n"
		                         : "snapshot failed; see DoomV's console\n";
	} else {
		out = "DoomV monitor commands:\n"
		      "  monitor steps             the step count: the -stopat / -snapshotat number of this point\n"
		      "  monitor snapshot <dir>    save the machine here, for -restore=<dir>\n";
	}
	gdb->send("O" + text_hex(out));
	gdb->send("OK");
}

void DoomSystem::gdb_handle(const std::string &p)
{
	if (p.empty()) { gdb->send(""); return; }
	const char c = p[0];
	if (p.rfind("qSupported", 0) == 0) {
		gdb->send("PacketSize=4000;qXfer:features:read+;swbreak+;hwbreak+;QStartNoAckMode+;vContSupported+");
	} else if (p == "QStartNoAckMode") {
		gdb->send("OK");
		gdb->set_no_ack();
	} else if (p.rfind(XFER_TARGET, 0) == 0) {
		const std::string xml = gdb_target_xml();
		const std::string range = p.substr(sizeof XFER_TARGET - 1);
		const size_t comma = range.find(',');
		const size_t off = std::strtoull(range.substr(0, comma).c_str(), nullptr, 16);
		const size_t len = std::strtoull(range.substr(comma + 1).c_str(), nullptr, 16);
		if (off >= xml.size()) gdb->send("l");
		else gdb->send((off + len >= xml.size() ? "l" : "m") + xml.substr(off, len));
	} else if (p == "?") {
		gdb->send(gdb_stop_reply(5));
	} else if (p == "qAttached") {
		gdb->send("1");
	} else if (p == "qC") {
		gdb->send("QC" + std::to_string(gdb_hart + 1));
	} else if (p == "qfThreadInfo") {
		std::string s = "m";
		char buf[16];
		for (unsigned i = 0; i < hart_count(); i++) {
			std::snprintf(buf, sizeof buf, "%s%x", i ? "," : "", i + 1);
			s += buf;
		}
		gdb->send(s);
	} else if (p == "qsThreadInfo") {
		gdb->send("l");
	} else if (p.rfind("qThreadExtraInfo,", 0) == 0) {
		const unsigned t = (unsigned)std::strtoul(p.c_str() + 17, nullptr, 16);
		gdb->send(text_hex("hart " + std::to_string(t ? t - 1 : 0)));
	} else if (c == 'H') {
		// Hg<thread>: whose registers. -1 and 0 mean "any": keep the one we have.
		const long t = std::strtol(p.c_str() + 2, nullptr, 16);
		if (p[1] == 'g' && t > 0 && (unsigned)t <= hart_count()) gdb_hart = (unsigned)t - 1;
		gdb->send("OK");
	} else if (c == 'T') {
		const unsigned long t = std::strtoul(p.c_str() + 1, nullptr, 16);
		gdb->send(t >= 1 && t <= hart_count() ? "OK" : "E01");
	} else if (c == 'g') {
		std::string all, one;
		for (unsigned i = 0; i <= REG_PC; i++) { gdb_read_reg(i, one); all += one; }
		gdb->send(all);
	} else if (c == 'G') {
		for (unsigned i = 0; i <= REG_PC && (i + 1) * 16 <= p.size() - 1; i++)
			gdb_write_reg(i, p.substr(1 + i * 16, 16));
		gdb->send("OK");
	} else if (c == 'p') {
		std::string hex;
		gdb->send(gdb_read_reg((unsigned)std::strtoul(p.c_str() + 1, nullptr, 16), hex) ? hex : "E01");
	} else if (c == 'P') {
		const size_t eq = p.find('=');
		const unsigned n = (unsigned)std::strtoul(p.substr(1, eq - 1).c_str(), nullptr, 16);
		gdb->send(eq != std::string::npos && gdb_write_reg(n, p.substr(eq + 1)) ? "OK" : "E01");
	} else if (c == 'm') {
		const size_t comma = p.find(',');
		uint64_t addr = std::strtoull(p.substr(1, comma - 1).c_str(), nullptr, 16);
		const uint64_t len = std::strtoull(p.substr(comma + 1).c_str(), nullptr, 16);
		std::string out;
		for (uint64_t i = 0; i < len; i++, addr++) {
			uint64_t pa;
			if (!gdb_translate(addr, pa) || !Memory::in_ram(pa, 1)) break;
			const uint8_t b = memory.ram_data()[pa - Memory::RAM_BASE];
			out.push_back(HEX[b >> 4]);
			out.push_back(HEX[b & 0xF]);
		}
		gdb->send(out.empty() && len ? "E14" : out);
	} else if (c == 'M') {
		const size_t comma = p.find(','), colon = p.find(':');
		uint64_t addr = std::strtoull(p.substr(1, comma - 1).c_str(), nullptr, 16);
		std::vector<uint8_t> data;
		bool ok = colon != std::string::npos && from_hex(p.substr(colon + 1), data);
		for (size_t i = 0; ok && i < data.size(); i++, addr++) {
			uint64_t pa;
			ok = gdb_translate(addr, pa) && Memory::in_ram(pa, 1);
			// Straight into RAM: fetches read RAM live, and the decoder's
			// cache is keyed by the encoding, so new code is seen at once.
			if (ok) memory.ram_data_mut()[pa - Memory::RAM_BASE] = data[i];
		}
		gdb->send(ok ? "OK" : "E14");
	} else if ((c == 'Z' || c == 'z') && p.size() > 3 && (p[1] == '0' || p[1] == '1')) {
		const uint64_t addr = std::strtoull(p.c_str() + 3, nullptr, 16);
		if (c == 'Z') {
			debugger.add_breakpoint(addr);
			gdb_breakpoints.push_back(addr);
		} else {
			debugger.remove_breakpoint(addr);
			const auto it = std::find(gdb_breakpoints.begin(), gdb_breakpoints.end(), addr);
			if (it != gdb_breakpoints.end()) gdb_breakpoints.erase(it);
		}
		gdb->send("OK");
	} else if (p == "vCont?") {
		gdb->send("vCont;c;C;s;S");
	} else if (c == 'c' || c == 's' || c == 'C' || c == 'S' || p.rfind("vCont;", 0) == 0) {
		// One step for s/S, or a vCont that steps any thread; otherwise run.
		const bool step = c == 's' || c == 'S' || (c == 'v' && (p.find(";s") != std::string::npos
		                                                        || p.find(";S") != std::string::npos));
		if (c == 'c' || c == 's') {
			if (p.size() > 1) harts[gdb_hart]->regs.set_pc(std::strtoull(p.c_str() + 1, nullptr, 16));
		}
		if (step) halt_at = memory.instruction_count() + 1;
		gdb_running = true;
		gdb_signal = 5;
		resume_from_halt();
	} else if (c == 'D') {
		for (uint64_t a : gdb_breakpoints) debugger.remove_breakpoint(a);
		gdb_breakpoints.clear();
		gdb->send("OK");
		gdb_running = false;
		resume_from_halt();
	} else if (c == 'k') {
		std::cout << "gdb: killed" << std::endl;
		std::exit(0);
	} else if (p.rfind("qRcmd,", 0) == 0) {
		std::vector<uint8_t> raw;
		from_hex(p.substr(6), raw);
		gdb_monitor(std::string(raw.begin(), raw.end()));
	} else if (p == "qSymbol::") {
		gdb->send("OK");
	} else {
		gdb->send("");   // not supported: gdb falls back or does without
	}
}
