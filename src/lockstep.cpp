// Commit traces, and lock-stepping against the golden reference.
//
// DoomV's golden reference is Sail, the RISC-V model generated from the same
// source the architecture is specified in. So the trace format here is
// Sail's: what sail_riscv_sim writes with
//
//   --trace-instr --trace-gpr --trace-fpr --trace-vreg --trace-csr
//   --trace-mem --trace-exception --trace-interrupt
//
// which is a line per instruction followed by a line per effect:
//
//   [36] [M]: 0x00000000800000E0 (0x30529073) csrrw x0, mtvec, x5
//   CSR mtvec (0x305) <- 0x00000000800000E8
//   [83] [U]: 0x00000000800001BC (0x00113023) sd x1, 0x0(x2)
//   mem[W,0x0000000080002000] <- 0x00AA00AA00AA00AA
//   [37] [M]: 0x00000000800000E4 (0x74445073) csrrwi x0, 0x744, 0x8
//   trapping from M to M to handle illegal-instruction
//   handling exc#illegal-instruction at priv M | tval=0x0000000074445073 | tval2=... | tinst=...
//   CSR mcause (0x342) <- 0x0000000000000002
//
// -trace=<path> writes DoomV's run in that format (without the disassembly
// and symbol columns). -lockstep=<path> runs DoomV against a reference trace
// in it -- Sail's, or an RTL testbench's in the same shape -- one record at a
// time, and halts at the first record that does not match: both records, the
// field that differs, and the machine state in crash.log. Headless, the exit
// status is 1 on a mismatch. A trace in Spike's --log-commits format is also
// read, for references that only produce that, but Sail's is the one DoomV
// is held to.
//
// Compared, for an instruction that completes: privilege (with V), pc and
// instruction bits; every register and CSR write, against DoomV's value
// after it; every store, by physical address and value; and that DoomV
// wrote no register and made no store that the reference did not.
// For an instruction that traps, or an interrupt: that DoomV took the same
// trap, with the same cause, epc and tval, and that the CSRs trap entry
// writes -- mstatus, mcause, mepc, mtval and the rest -- hold the same values.
//
// -lockstep-strict compares everything, and is how DoomV is held to Sail:
// run with Sail's own configuration, unmodified, DoomV has to produce Sail's
// trace exactly -- counter and time reads, pending-interrupt state, and
// interrupts DoomV takes by itself at the instruction Sail took them.
//
// Without it, lock-step is lenient about what an implementation's own clock
// and devices decide rather than the instruction set, so that an RTL design
// with a different timer can still be stepped: reads of the counters and the
// time, of pending-interrupt state (mip, sip, the topi and topei registers,
// hgeip, seed), and loads from anything that is not RAM are taken from the
// reference, and interrupts are taken exactly where the reference took them,
// never on DoomV's own, checking that they were enabled there. The summary
// counts how many values were taken; in strict mode it is always zero.

#include "doom_system.hpp"
#include "extensions.hpp"
#include "mmu.hpp"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t INTERRUPT_BIT = 1ull << 63;

// ---- names -----------------------------------------------------------------

// Sail's exceptionType_to_str and interruptType_to_str (model/core/types.sail).
struct Named { uint64_t cause; const char *name; };
const Named sail_exceptions[] = {
	{0, "misaligned-fetch"}, {1, "fetch-access-fault"}, {2, "illegal-instruction"},
	{3, "software-breakpoint"}, {3, "hardware-breakpoint"},
	{4, "misaligned-load"}, {5, "load-access-fault"},
	{6, "misaligned-store/amo"}, {7, "store/amo-access-fault"},
	{8, "u-call"}, {9, "s-call"}, {10, "vs-call"}, {11, "m-call"},
	{12, "fetch-page-fault"}, {13, "load-page-fault"}, {15, "store/amo-page-fault"},
	{18, "software-check-exception"}, {19, "hardware-error-exception"},
	{20, "fetch-guest-page-fault"}, {21, "load-guest-page-fault"},
	{22, "virtual-instruction"}, {23, "store/amo-guest-page-fault"},
};
const Named sail_interrupts[] = {
	{1, "supervisor-software-interrupt"}, {2, "virtual-supervisor-software-interrupt"},
	{3, "machine-software-interrupt"}, {5, "supervisor-timer-interrupt"},
	{6, "virtual-supervisor-timer-interrupt"}, {7, "machine-timer-interrupt"},
	{9, "supervisor-external-interrupt"}, {10, "virtual-supervisor-external-interrupt"},
	{11, "machine-external-interrupt"}, {12, "supervisor guest-external-interrupt"},
	{13, "counter-overflow interrupt"},
};
// Spike's trap class names (riscv/trap.h), for Spike-format references.
const Named spike_exceptions[] = {
	{0, "instruction_address_misaligned"}, {1, "instruction_access_fault"},
	{2, "illegal_instruction"}, {3, "breakpoint"},
	{4, "load_address_misaligned"}, {5, "load_access_fault"},
	{6, "store_address_misaligned"}, {7, "store_access_fault"},
	{8, "user_ecall"}, {9, "supervisor_ecall"}, {10, "virtual_supervisor_ecall"}, {11, "machine_ecall"},
	{12, "instruction_page_fault"}, {13, "load_page_fault"}, {15, "store_page_fault"},
	{16, "double_trap"}, {18, "software_check"},
	{20, "instruction_guest_page_fault"}, {21, "load_guest_page_fault"},
	{22, "virtual_instruction"}, {23, "store_guest_page_fault"},
};

std::string sail_trap_name(uint64_t cause, bool interrupt)
{
	if (interrupt) {
		for (const Named &n : sail_interrupts) if (n.cause == cause) return n.name;
		return "reserved-interrupt-" + std::to_string(cause);
	}
	for (const Named &n : sail_exceptions) if (n.cause == cause) return n.name;
	return "reserved-" + std::to_string(cause);
}

bool lookup(const Named *table, size_t count, const std::string &name, uint64_t &cause)
{
	for (size_t i = 0; i < count; i++)
		if (name == table[i].name) { cause = table[i].cause; return true; }
	return false;
}

bool is_fetch_fault(uint64_t cause)
{
	return cause == 1 || cause == 12 || cause == 20;
}

// Values an implementation's own clock and devices decide. See the top.
bool reference_decides_csr(uint16_t c)
{
	if (c >= 0xB00 && c <= 0xB1F) return true;   // mcycle, minstret, mhpmcounter3..31
	if (c >= 0xB80 && c <= 0xB9F) return true;   // their RV32 high halves
	if (c >= 0xC00 && c <= 0xC1F) return true;   // cycle, time, instret, hpmcounter3..31
	if (c >= 0xC80 && c <= 0xC9F) return true;
	switch (c) {
	case 0x344: case 0x144: case 0x244: case 0x644:   // mip, sip, vsip, hip
	case 0xFB0: case 0xDB0: case 0xEB0:               // mtopi, stopi, vstopi
	case 0x35C: case 0x15C: case 0x25C:               // mtopei, stopei, vstopei
	case 0xE12:                                       // hgeip
	case 0x015:                                       // seed
		return true;
	default:
		return false;
	}
}

const char *csr_label(uint16_t c)
{
	switch (c) {
	case 0x001: return "fflags";     case 0x002: return "frm";        case 0x003: return "fcsr";
	case 0x008: return "vstart";     case 0x009: return "vxsat";      case 0x00A: return "vxrm";
	case 0x00F: return "vcsr";       case 0xC20: return "vl";         case 0xC21: return "vtype";
	case 0x100: return "sstatus";    case 0x104: return "sie";        case 0x105: return "stvec";
	case 0x106: return "scounteren"; case 0x10A: return "senvcfg";    case 0x140: return "sscratch";
	case 0x141: return "sepc";       case 0x142: return "scause";     case 0x143: return "stval";
	case 0x14D: return "stimecmp";   case 0x180: return "satp";
	case 0x300: return "mstatus";    case 0x301: return "misa";       case 0x302: return "medeleg";
	case 0x303: return "mideleg";    case 0x304: return "mie";        case 0x305: return "mtvec";
	case 0x306: return "mcounteren"; case 0x30A: return "menvcfg";    case 0x340: return "mscratch";
	case 0x341: return "mepc";       case 0x342: return "mcause";     case 0x343: return "mtval";
	case 0x34A: return "mtinst";     case 0x34B: return "mtval2";
	case 0x600: return "hstatus";    case 0x602: return "hedeleg";    case 0x603: return "hideleg";
	case 0x680: return "hgatp";      case 0x200: return "vsstatus";   case 0x241: return "vsepc";
	case 0x242: return "vscause";    case 0x243: return "vstval";     case 0x280: return "vsatp";
	case 0x7A0: return "tselect";
	default:
		if (c >= 0x3A0 && c <= 0x3AF) return "pmpcfg";
		if (c >= 0x3B0 && c <= 0x3EF) return "pmpaddr";
		return "csr";
	}
}

// Sail's privilege names: S is "HS" when the hart has H.
std::string priv_name(int priv, bool virt)
{
	if (priv == 3) return "M";
	if (priv == 1) return virt ? "VS" : (Extensions.H ? "HS" : "S");
	return virt ? "VU" : "U";
}

bool parse_priv_name(const std::string &s, int &priv, bool &virt)
{
	virt = false;
	if (s == "M") { priv = 3; return true; }
	if (s == "S" || s == "HS") { priv = 1; return true; }
	if (s == "U") { priv = 0; return true; }
	if (s == "VS") { priv = 1; virt = true; return true; }
	if (s == "VU") { priv = 0; virt = true; return true; }
	return false;
}

// ---- numbers ----------------------------------------------------------------

std::string hex(uint64_t v, int digits)
{
	char buf[24];
	std::snprintf(buf, sizeof(buf), "0x%0*" PRIX64, digits, v);
	return buf;
}

bool parse_hex(const std::string &s, uint64_t &v)
{
	if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
	char *end = nullptr;
	v = std::strtoull(s.c_str() + 2, &end, 16);
	return end && *end == '\0';
}

// "0x..." to little-endian bytes, for values wider than 64 bits.
std::vector<uint8_t> hex_bytes(const std::string &s)
{
	std::vector<uint8_t> out;
	if (s.size() < 3) return out;
	std::string digits = s.substr(2);
	if (digits.size() % 2) digits.insert(digits.begin(), '0');
	for (size_t i = digits.size(); i >= 2; i -= 2)
		out.push_back((uint8_t)std::strtoul(digits.substr(i - 2, 2).c_str(), nullptr, 16));
	return out;
}

bool is_index_token(const std::string &t, char prefix, unsigned &idx)
{
	if (t.size() < 2 || t[0] != prefix) return false;
	for (size_t i = 1; i < t.size(); i++) if (t[i] < '0' || t[i] > '9') return false;
	idx = (unsigned)std::strtoul(t.c_str() + 1, nullptr, 10);
	return true;
}

// ---- records ----------------------------------------------------------------

struct RefField { char cls; unsigned idx; std::string value; };   // x f v c
struct RefStore { uint64_t addr; std::string value; };

struct RefRecord {
	enum Kind { Commit, Exception, Interrupt } kind = Commit;
	std::vector<std::pair<uint64_t, std::string>> lines;   // line number, text
	bool has_insn = false;
	int priv = 0;
	bool virt = false;
	uint64_t pc = 0, insn = 0;
	size_t insn_digits = 8;
	std::vector<RefField> fields;        // written by the instruction
	std::vector<RefField> trap_fields;   // CSRs written by trap entry
	std::vector<RefStore> stores;
	bool physical = false;               // store addresses are physical (Sail) or virtual (Spike)
	bool trap_started = false;
	uint64_t cause = 0, epc = 0, tval = 0;
	bool has_epc = false, has_tval = false;
};

bool starts(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }

bool is_epc_csr(unsigned c) { return c == 0x341 || c == 0x141 || c == 0x241; }

} // namespace

struct DoomSystem::LockstepState {
	std::ifstream in;
	uint64_t line_no = 0;
	enum Format { Unknown, Sail, Spike } format = Unknown;
	bool have_peek = false;
	std::string peek;
	uint64_t peek_line = 0;
	std::deque<RefRecord> ready;
	RefRecord cur;
	bool have_cur = false;
	bool synced = false;
	bool done = false;
	uint64_t matched = 0, overrides = 0, skipped = 0;

	bool read_line(std::string &s, uint64_t &n)
	{
		if (have_peek) { s = peek; n = peek_line; have_peek = false; return true; }
		if (!std::getline(in, s)) return false;
		while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
		n = ++line_no;
		return true;
	}

	bool next(RefRecord &r)
	{
		if (!ready.empty()) { r = ready.front(); ready.pop_front(); return true; }
		return format == Spike ? next_spike(r) : next_sail(r);
	}

	void push_back(const RefRecord &r) { ready.push_front(r); }

	// ---- Sail ---------------------------------------------------------------

	// A record ends where the next begins, so a finished one is handed back
	// when its successor's first line arrives.
	void finish(RefRecord &rec)
	{
		// A fetch fault has no instruction of its own: it is the step after
		// the last instruction line. Tell the two apart by where it trapped.
		if (rec.kind == RefRecord::Exception && rec.has_insn && is_fetch_fault(rec.cause)
		    && rec.has_epc && rec.epc != rec.pc) {
			RefRecord trap = rec;
			trap.has_insn = false;
			trap.fields.clear();
			trap.stores.clear();
			rec.kind = RefRecord::Commit;
			rec.trap_fields.clear();
			rec.trap_started = false;
			ready.push_back(rec);
			ready.push_back(trap);
			return;
		}
		ready.push_back(rec);
	}

	bool next_sail(RefRecord &out)
	{
		std::string line;
		uint64_t n = 0;
		while (read_line(line, n)) {
			if (line.empty()) continue;

			// [N] [P]: 0xPC (0xINSN) disassembly  symbol
			if (line[0] == '[') {
				const size_t p1 = line.find("] [");
				const size_t p2 = line.find("]: 0x");
				const size_t open = line.find(" (0x");
				const size_t close = (open == std::string::npos) ? std::string::npos : line.find(')', open);
				int priv = 0;
				bool virt = false;
				uint64_t pc = 0, insn = 0;
				if (p1 == std::string::npos || p2 == std::string::npos || open == std::string::npos || close == std::string::npos) continue;
				if (!parse_priv_name(line.substr(p1 + 3, p2 - p1 - 3), priv, virt)) continue;
				if (!parse_hex(line.substr(p2 + 3, open - p2 - 3), pc)) continue;
				const std::string insn_text = line.substr(open + 2, close - open - 2);
				if (!parse_hex(insn_text, insn)) continue;
				format = Sail;
				if (have_cur) finish(cur);
				cur = RefRecord{};
				cur.physical = true;
				cur.has_insn = true;
				cur.priv = priv;
				cur.virt = virt;
				cur.pc = pc;
				cur.insn = insn;
				cur.insn_digits = insn_text.size() - 2;
				cur.lines.push_back({n, line});
				have_cur = true;
				if (!ready.empty()) { out = ready.front(); ready.pop_front(); return true; }
				continue;
			}

			if (starts(line, "core")) {
				// Not Sail after all.
				if (format == Unknown) {
					format = Spike;
					have_peek = true;
					peek = line;
					peek_line = n;
					return next_spike(out);
				}
				continue;
			}
			if (!have_cur && !starts(line, "handling ")) continue;   // before the first instruction

			if (starts(line, "handling int#") || starts(line, "handling exc#")) {
				const bool interrupt = starts(line, "handling int#");
				const size_t name_at = 13;
				const size_t at = line.find(" at priv ");
				const size_t tval_at = line.find("| tval=");
				if (at == std::string::npos) continue;
				const std::string name = line.substr(name_at, at - name_at);
				uint64_t cause = 0;
				const bool known = interrupt
					? lookup(sail_interrupts, sizeof(sail_interrupts) / sizeof(sail_interrupts[0]), name, cause)
					: lookup(sail_exceptions, sizeof(sail_exceptions) / sizeof(sail_exceptions[0]), name, cause);
				if (!known) continue;
				uint64_t tval = 0;
				bool has_tval = false;
				if (tval_at != std::string::npos) {
					const size_t end = line.find(' ', tval_at + 7);
					has_tval = parse_hex(line.substr(tval_at + 7, end == std::string::npos ? std::string::npos : end - tval_at - 7), tval);
				}
				format = Sail;
				if (interrupt || !have_cur || cur.trap_started) {
					// An interrupt, or a trap with no instruction line of its
					// own: a new step.
					if (have_cur) finish(cur);
					cur = RefRecord{};
					cur.physical = true;
					have_cur = true;
				}
				cur.kind = interrupt ? RefRecord::Interrupt : RefRecord::Exception;
				cur.trap_started = true;
				cur.cause = cause;
				cur.tval = tval;
				cur.has_tval = has_tval;
				cur.lines.push_back({n, line});
				if (!ready.empty()) { out = ready.front(); ready.pop_front(); return true; }
				continue;
			}

			if (starts(line, "CSR ")) {
				const size_t open = line.find(" (0x");
				const size_t arrow = line.find(") <- ");
				if (open == std::string::npos || arrow == std::string::npos) continue;   // a read
				uint64_t num = 0;
				if (!parse_hex(line.substr(open + 2, arrow - open - 2), num)) continue;
				const std::string value = line.substr(arrow + 5);
				if (cur.trap_started) {
					cur.trap_fields.push_back({'c', (unsigned)num, value});
					if (is_epc_csr((unsigned)num)) {
						uint64_t epc = 0;
						if (parse_hex(value, epc)) { cur.epc = epc; cur.has_epc = true; }
					}
				} else {
					cur.fields.push_back({'c', (unsigned)num, value});
				}
				cur.lines.push_back({n, line});
				continue;
			}

			if (starts(line, "mem[W,")) {
				const size_t close = line.find("] <- ");
				uint64_t addr = 0;
				if (close == std::string::npos || !parse_hex(line.substr(6, close - 6), addr)) continue;
				if (!cur.trap_started) cur.stores.push_back({addr, line.substr(close + 5)});
				cur.lines.push_back({n, line});
				continue;
			}

			// x5 <- 0x..., f10 <- 0x..., v2 <- 0x...
			const size_t arrow = line.find(" <- 0x");
			unsigned idx = 0;
			if (arrow != std::string::npos) {
				const std::string reg = line.substr(0, arrow);
				if (is_index_token(reg, 'x', idx) || is_index_token(reg, 'f', idx) || is_index_token(reg, 'v', idx)) {
					if (!cur.trap_started) cur.fields.push_back({reg[0], idx, line.substr(arrow + 4)});
					cur.lines.push_back({n, line});
				}
			}
			// Everything else -- fetches, loads, CSR reads, "trapping from",
			// "ret-ing from", CLINT and HTIF lines -- needs no comparing.
		}
		if (have_cur) { finish(cur); have_cur = false; }
		if (!ready.empty()) { out = ready.front(); ready.pop_front(); return true; }
		return false;
	}

	// ---- Spike --------------------------------------------------------------

	static bool core_body(const std::string &line, std::string &body)
	{
		if (line.compare(0, 4, "core") != 0) return false;
		const size_t colon = line.find(':');
		if (colon == std::string::npos) return false;
		const size_t start = line.find_first_not_of(' ', colon + 1);
		body = (start == std::string::npos) ? std::string() : line.substr(start);
		return true;
	}

	bool next_spike(RefRecord &r)
	{
		std::string line, body;
		uint64_t n = 0;
		while (read_line(line, n)) {
			if (!core_body(line, body)) continue;
			r = RefRecord{};
			r.lines.push_back({n, line});

			if (starts(body, "exception ")) {
				const size_t at = body.find(", epc ");
				if (at == std::string::npos) continue;
				const std::string name = body.substr(10, at - 10);
				if (starts(name, "interrupt #")) {
					r.kind = RefRecord::Interrupt;
					r.cause = std::strtoull(name.c_str() + 11, nullptr, 10);
				} else if (starts(name, "trap #")) {
					r.kind = RefRecord::Exception;
					r.cause = std::strtoull(name.c_str() + 6, nullptr, 10);
				} else if (starts(name, "trap_") &&
				           lookup(spike_exceptions, sizeof(spike_exceptions) / sizeof(spike_exceptions[0]), name.substr(5), r.cause)) {
					r.kind = RefRecord::Exception;
				} else {
					continue;
				}
				if (!parse_hex(body.substr(at + 6), r.epc)) continue;
				r.has_epc = true;
				std::string next_line, next_body;
				uint64_t nn = 0;
				if (read_line(next_line, nn)) {
					uint64_t tv = 0;
					if (core_body(next_line, next_body) && starts(next_body, "tval ") && parse_hex(next_body.substr(5), tv)) {
						r.has_tval = true;
						r.tval = tv;
						r.lines.push_back({nn, next_line});
					} else {
						have_peek = true;
						peek = next_line;
						peek_line = nn;
					}
				}
				return true;
			}

			std::istringstream ss(body);
			std::vector<std::string> t;
			for (std::string w; ss >> w;) t.push_back(w);
			if (t.size() < 3 || t[0].size() != 1 || t[0][0] < '0' || t[0][0] > '3') continue;
			if (!parse_hex(t[1], r.pc)) continue;
			if (t[2].size() < 4 || t[2][0] != '(' || t[2].back() != ')') continue;
			const std::string insn = t[2].substr(1, t[2].size() - 2);
			if (!parse_hex(insn, r.insn)) continue;
			r.has_insn = true;
			r.priv = t[0][0] - '0';
			r.insn_digits = insn.size() - 2;
			for (size_t i = 3; i < t.size(); i++) {
				unsigned idx = 0;
				if (t[i] == "mem" && i + 1 < t.size()) {
					uint64_t addr = 0;
					if (!parse_hex(t[i + 1], addr)) continue;
					if (i + 2 < t.size() && starts(t[i + 2], "0x")) { r.stores.push_back({addr, t[i + 2]}); i += 2; }
					else i += 1;
				} else if ((is_index_token(t[i], 'x', idx) || is_index_token(t[i], 'f', idx) || is_index_token(t[i], 'v', idx))
				           && i + 1 < t.size()) {
					r.fields.push_back({t[i][0], idx, t[i + 1]});
					i += 1;
				} else if (t[i][0] == 'c' && t[i].find('_') != std::string::npos && i + 1 < t.size()) {
					const std::string num = t[i].substr(1, t[i].find('_') - 1);
					if (!num.empty() && num.find_first_not_of("0123456789") == std::string::npos) {
						r.fields.push_back({'c', (unsigned)std::strtoul(num.c_str(), nullptr, 10), t[i + 1]});
						i += 1;
					}
				}
			}
			return true;
		}
		return false;
	}
};

bool DoomSystem::set_trace(const char *path)
{
	trace_file = std::fopen(path, "w");
	if (!trace_file) return false;
	tracing = true;
	return true;
}

bool DoomSystem::set_lockstep(const char *path)
{
	lock = new LockstepState();
	lock->in.open(path);
	if (!lock->in) return false;
	lockstep_active = true;
	tracing = true;
	return true;
}

void DoomSystem::lockstep_end()
{
	if (lock->done) return;
	lock->done = true;
	console_drain();
	if (!lock->synced) {
		std::cout << "lockstep: the reference never reached this machine's starting pc "
		          << hex(regs.get_pc(), 16) << std::endl;
		lockstep_failed = true;
	} else {
		std::cout << "lockstep: the reference ended after " << lock->matched
		          << " records, all matching (" << lock->overrides
		          << " values taken from the reference"
		          << (lockstep_strict ? ", strict" : "") << ")" << std::endl;
	}
	debugger.halted = true;
	run_finished = true;
}

void DoomSystem::lockstep_report()
{
	if (!lock || lock->done || lockstep_failed) return;
	console_drain();
	std::cout << "lockstep: " << lock->matched << " records matched before the run ended ("
	          << lock->overrides << " values taken from the reference"
	          << (lockstep_strict ? ", strict" : "") << ")" << std::endl;
}

void DoomSystem::traced_step()
{
	if (debugger.halted) return;

	RefRecord ref;
	bool have_ref = false;
	if (lockstep_active) {
		for (;;) {
			if (!lock->next(ref)) { lockstep_end(); return; }
			if (lock->synced) break;
			if (ref.kind == RefRecord::Commit && ref.has_insn && ref.pc == regs.get_pc()) {
				lock->synced = true;
				if (lock->skipped) {
					console_drain();
					std::cout << "lockstep: skipped " << lock->skipped
					          << " reference records before this machine's starting pc" << std::endl;
				}
				break;
			}
			lock->skipped++;
		}
		have_ref = true;
	}

	std::vector<std::string> actual;   // DoomV's record of the step, in the trace format
	const auto fail = [&](const std::string &why) {
		console_drain();
		const uint64_t first = ref.lines.empty() ? 0 : ref.lines.front().first;
		std::cout << "lockstep: MISMATCH at reference line " << first << ", after " << lock->matched
		          << " matching records (instruction " << memory.instruction_count() << ")\n"
		          << "  " << why << "\n  reference:\n";
		for (size_t i = 0; i < ref.lines.size() && i < 16; i++) std::cout << "    " << ref.lines[i].second << "\n";
		std::cout << "  DoomV:\n";
		for (size_t i = 0; i < actual.size() && i < 16; i++) std::cout << "    " << actual[i] << "\n";
		debugger.halted = true;
		debugger.dump_log(regs, memory, "crash.log");
		std::cout << "  machine state in crash.log" << std::endl;
		lockstep_failed = true;
		run_finished = true;
	};

	// The value a trace records for a CSR write: what the CSR now holds. That
	// is its read, except for tselect, which holds what was written and reads
	// back its inverse to say there are no triggers.
	const auto logged_csr = [&](uint16_t c) -> uint64_t {
		if (c == 0x7A0) return regs.read_csr(0x7A0);
		return core.read_csr_effective(regs, memory, c);
	};

	// The clock moves at the end of a step, and Sail's trace shows both sides
	// of that: a CSR write is logged as it happens, before the tick, while the
	// mip change the tick causes is logged by the next step's update_mip, at
	// the end of this record. So a CSR this step wrote is compared, the first
	// time the reference names it, with what it held when written; anything
	// else with what it holds now, after the clock.
	std::map<uint16_t, uint64_t> written_csr;
	std::map<uint16_t, int> csr_seen;
	const auto step_csr = [&](uint16_t c) -> uint64_t {
		const auto it = written_csr.find(c);
		if (it != written_csr.end() && csr_seen[c]++ == 0) return it->second;
		return logged_csr(c);
	};

	// Whether a CSR's value is the reference's to decide. Never, in strict mode.
	const auto from_ref_csr = [&](uint16_t c) { return !lockstep_strict && reference_decides_csr(c); };

	// CSR values after a trap entry, against the reference's.
	const auto check_trap_csrs = [&](const std::vector<RefField> &fields) -> bool {
		for (const RefField &f : fields) {
			if (from_ref_csr((uint16_t)f.idx)) continue;
			uint64_t want = 0;
			if (!parse_hex(f.value, want)) continue;
			const uint64_t mine = step_csr((uint16_t)f.idx);
			if (mine != want) {
				fail("after trap entry, CSR " + std::string(csr_label((uint16_t)f.idx)) + " (" + hex(f.idx, 3)
				     + "): reference " + hex(want, 16) + ", DoomV " + hex(mine, 16));
				return false;
			}
		}
		return true;
	};

	const auto trace_csr_lines = [&](const std::vector<uint16_t> &writes, std::vector<std::string> &out) {
		std::set<uint16_t> seen;
		for (uint16_t c : writes) {
			if (reference_decides_csr(c) || !seen.insert(c).second) continue;
			char buf[96];
			const auto it = written_csr.find(c);
			std::snprintf(buf, sizeof(buf), "CSR %s (0x%03X) <- 0x%016" PRIX64, csr_label(c), (unsigned)c,
			              it != written_csr.end() ? it->second : logged_csr(c));
			out.push_back(buf);
		}
	};

	const auto emit = [&]() {
		if (!trace_file) return;
		for (const std::string &l : actual) { std::fputs(l.c_str(), trace_file); std::fputc('\n', trace_file); }
	};

	// ---- an interrupt the reference took -------------------------------------
	// Lenient: the reference times it, so take it here. Strict: DoomV's own
	// interrupt logic has to take it at this step, which the trap comparison
	// below checks.
	if (have_ref && ref.kind == RefRecord::Interrupt && !lockstep_strict) {
		const uint64_t pc = regs.get_pc();
		if (!ref.has_epc || pc != ref.epc) {
			actual.push_back("(at " + hex(pc, 16) + ", no interrupt)");
			fail("the reference took " + sail_trap_name(ref.cause, true) + " at a different pc");
			return;
		}
		if (!core.interrupt_enabled(regs, (int)ref.cause)) {
			actual.push_back("(at " + hex(pc, 16) + ", no interrupt)");
			fail("the reference took " + sail_trap_name(ref.cause, true) + ", which DoomV has disabled here");
			return;
		}
		std::vector<uint16_t> csr_writes;
		regs.csr_log = &csr_writes;
		core.take_interrupt(regs, (int)ref.cause);
		regs.csr_log = nullptr;
		memory.step_instructions(1);
		step_committed = false;
		end_step();
		actual.push_back("handling int#" + sail_trap_name(ref.cause, true) + " at priv "
		                 + priv_name((int)regs.get_priv(), regs.get_virt())
		                 + " | tval=0x0000000000000000 | tval2=0x0000000000000000 | tinst=0x0000000000000000");
		trace_csr_lines(csr_writes, actual);
		emit();
		if (!check_trap_csrs(ref.trap_fields)) return;
		lock->matched++;
		return;
	}

	// ---- one step --------------------------------------------------------------
	const int VB = Registers::VLEN_BYTES;
	uint64_t x0[32], f0[32];
	uint8_t v0[32 * Registers::VLEN_BYTES];
	for (int i = 0; i < 32; i++) {
		x0[i] = regs.read_x(i);
		const double d = regs.read_f(i);
		std::memcpy(&f0[i], &d, sizeof(double));
		std::memcpy(v0 + i * VB, regs.read_v(i), VB);
	}
	const uint8_t fflags0 = regs.get_fflags(), frm0 = regs.get_frm();
	const int priv0 = (int)regs.get_priv();
	const bool virt0 = regs.get_virt();
	const uint64_t pc0 = regs.get_pc();
	const uint64_t traps0 = core.trap_count;
	const uint64_t step_no = memory.instruction_count();

	std::vector<AccessRecord> access;
	std::vector<std::pair<uint64_t, uint8_t>> stored;
	std::vector<uint16_t> csr_writes;
	core.access_log = &access;
	memory.store_log = &stored;
	regs.csr_log = &csr_writes;
	step_execute();
	core.access_log = nullptr;
	memory.store_log = nullptr;
	regs.csr_log = nullptr;
	for (uint16_t c : csr_writes) written_csr[c] = logged_csr(c);
	if (memory.instruction_count() != step_no) end_step();

	const bool trapped = core.trap_count != traps0;
	if (!step_committed && !trapped) {
		// Nothing ran: a breakpoint, or the run stopping. The record waits.
		if (have_ref) lock->push_back(ref);
		return;
	}

	std::map<uint64_t, uint8_t> store_bytes;
	for (const auto &b : stored) store_bytes[b.first] = b.second;
	const auto store_value = [&](uint64_t paddr, unsigned size, uint64_t &v) {
		v = 0;
		for (unsigned i = 0; i < size && i < 8; i++) {
			const auto it = store_bytes.find(paddr + i);
			if (it == store_bytes.end()) return false;
			v |= (uint64_t)it->second << (8 * i);
		}
		return true;
	};
	// Stores that wrote. An address can be translated for a store that then
	// does not happen -- a store-conditional that lost its reservation.
	std::vector<AccessRecord> writes;
	for (const AccessRecord &a : access) {
		uint64_t v = 0;
		if (a.store && store_value(a.paddr, a.size, v)) writes.push_back(a);
	}

	char buf[160];
	const bool interrupt_taken = trapped && core.last_trap_interrupt;
	if (step_decoded && !interrupt_taken) {
		std::snprintf(buf, sizeof(buf), "[%" PRIu64 "] [%s]: 0x%016" PRIX64 " (0x%0*" PRIX32 ")",
		              step_no, priv_name(priv0, virt0).c_str(), pc0, step_insn_len == 2 ? 4 : 8, step_insn);
		actual.push_back(buf);
	}
	if (step_committed) {
		for (int i = 1; i < 32; i++) {
			if (regs.read_x(i) == x0[i]) continue;
			std::snprintf(buf, sizeof(buf), "x%d <- 0x%016" PRIX64, i, regs.read_x(i));
			actual.push_back(buf);
		}
		for (int i = 0; i < 32; i++) {
			const double d = regs.read_f(i);
			uint64_t bits;
			std::memcpy(&bits, &d, sizeof(double));
			if (bits == f0[i]) continue;
			std::snprintf(buf, sizeof(buf), "f%d <- 0x%016" PRIX64, i, bits);
			actual.push_back(buf);
		}
		for (int i = 0; i < 32; i++) {
			const uint8_t *v = regs.read_v(i);
			if (std::memcmp(v, v0 + i * VB, VB) == 0) continue;
			std::string l = "v" + std::to_string(i) + " <- 0x";
			for (int b = VB - 1; b >= 0; b--) { std::snprintf(buf, sizeof(buf), "%02X", v[b]); l += buf; }
			actual.push_back(l);
		}
		std::vector<uint16_t> csrs;
		for (uint16_t c : csr_writes) if (c > 0x003) csrs.push_back(c);
		if (regs.get_fflags() != fflags0) csrs.push_back(0x001);
		if (regs.get_frm() != frm0) csrs.push_back(0x002);
		trace_csr_lines(csrs, actual);
		for (const AccessRecord &a : writes) {
			uint64_t v = 0;
			store_value(a.paddr, a.size, v);
			std::snprintf(buf, sizeof(buf), "mem[W,0x%016" PRIX64 "] <- 0x%0*" PRIX64, a.paddr, a.size * 2, v);
			actual.push_back(buf);
		}
	} else {
		const uint64_t cause = core.last_trap_cause & ~INTERRUPT_BIT;
		const std::string target = priv_name((int)regs.get_priv(), regs.get_virt());
		if (!interrupt_taken)
			actual.push_back("trapping from " + priv_name(priv0, virt0) + " to " + target + " to handle "
			                 + sail_trap_name(cause, false));
		std::snprintf(buf, sizeof(buf), " at priv %s | tval=0x%016" PRIX64 " | tval2=0x0000000000000000 | tinst=0x0000000000000000",
		              target.c_str(), interrupt_taken ? 0 : core.last_trap_tval);
		actual.push_back(std::string("handling ") + (interrupt_taken ? "int#" : "exc#")
		                 + sail_trap_name(cause, interrupt_taken) + buf);
		trace_csr_lines(csr_writes, actual);
	}
	emit();

	if (!have_ref) return;

	const auto check_priv_pc_insn = [&]() -> bool {
		if (priv0 != ref.priv || (ref.physical && virt0 != ref.virt)) { fail("different privilege"); return false; }
		if (pc0 != ref.pc) { fail("different pc"); return false; }
		const bool compressed = ref.insn_digits <= 4;
		if (!step_decoded || (step_insn_len == 2) != compressed || (uint64_t)step_insn != ref.insn) {
			fail("different instruction");
			return false;
		}
		return true;
	};

	// ---- the reference took an interrupt, and DoomV must have too (strict) -------
	if (ref.kind == RefRecord::Interrupt) {
		if (!interrupt_taken) {
			fail("the reference took " + sail_trap_name(ref.cause, true) + " here and DoomV did not");
			return;
		}
		const uint64_t cause = core.last_trap_cause & ~INTERRUPT_BIT;
		if (cause != ref.cause) {
			fail("different interrupt: the reference took " + sail_trap_name(ref.cause, true));
			return;
		}
		if (ref.has_epc && core.last_trap_epc != ref.epc) { fail("same interrupt, different epc"); return; }
		if (!check_trap_csrs(ref.trap_fields)) return;
		lock->matched++;
		return;
	}

	// ---- the reference trapped ---------------------------------------------------
	if (ref.kind == RefRecord::Exception) {
		if (step_committed) {
			fail("the reference took " + sail_trap_name(ref.cause, false) + " and DoomV did not trap");
			return;
		}
		if (ref.has_insn && !check_priv_pc_insn()) return;
		const uint64_t cause = core.last_trap_cause & ~INTERRUPT_BIT;
		if (core.last_trap_interrupt || cause != ref.cause) {
			fail("different trap: the reference took " + sail_trap_name(ref.cause, false));
			return;
		}
		const uint64_t want_epc = ref.has_epc ? ref.epc : ref.pc;
		if ((ref.has_epc || ref.has_insn) && core.last_trap_epc != want_epc) { fail("same trap, different epc"); return; }
		if (ref.has_tval && core.last_trap_tval != ref.tval) {
			fail("same trap, different tval: reference " + hex(ref.tval, 16) + ", DoomV " + hex(core.last_trap_tval, 16));
			return;
		}
		if (!check_trap_csrs(ref.trap_fields)) return;
		lock->matched++;
		return;
	}

	// ---- the reference completed an instruction ---------------------------------
	if (!step_committed) {
		fail(interrupt_taken ? "DoomV took an interrupt where the reference did not"
		                     : "DoomV trapped where the reference completed the instruction");
		return;
	}
	if (!check_priv_pc_insn()) return;

	// Whether this instruction's result is the reference's to decide: a read of
	// a counter, the time or interrupt state, or a load from a device.
	bool from_reference = false;
	if (!lockstep_strict) {
	const uint32_t opcode = step_insn & 0x7F, funct3 = (step_insn >> 12) & 7;
	if (step_insn_len == 4 && opcode == 0x73 && funct3 != 0 && funct3 != 4
	    && reference_decides_csr((uint16_t)((step_insn >> 20) & 0xFFF)))
		from_reference = true;
	for (const AccessRecord &a : access)
		if (!a.store && !memory.is_ram(a.paddr, a.size)) from_reference = true;
	}

	std::set<unsigned> ref_x, ref_f;
	for (const RefField &f : ref.fields) {
		if (f.cls == 'x') ref_x.insert(f.idx);
		if (f.cls == 'f') ref_f.insert(f.idx);
	}
	for (int i = 1; i < 32; i++) {
		if (regs.read_x(i) != x0[i] && !ref_x.count((unsigned)i)) {
			fail("DoomV wrote x" + std::to_string(i) + ", which the reference did not");
			return;
		}
		const double d = regs.read_f(i);
		uint64_t bits;
		std::memcpy(&bits, &d, sizeof(double));
		if (bits != f0[i] && !ref_f.count((unsigned)i)) {
			fail("DoomV wrote f" + std::to_string(i) + ", which the reference did not");
			return;
		}
	}

	for (const RefField &f : ref.fields) {
		uint64_t want = 0;
		if (f.cls != 'v' && !parse_hex(f.value, want)) { fail("cannot read reference value " + f.value); return; }
		if (f.cls == 'x') {
			if (f.idx == 0 || f.idx > 31) continue;
			if (from_reference) {
				if (regs.read_x((int)f.idx) != want) { regs.write_x((int)f.idx, want); lock->overrides++; }
				continue;
			}
			if (regs.read_x((int)f.idx) != want) {
				fail("x" + std::to_string(f.idx) + ": reference " + hex(want, 16) + ", DoomV " + hex(regs.read_x((int)f.idx), 16));
				return;
			}
		} else if (f.cls == 'f') {
			if (f.idx > 31) continue;
			const double d = regs.read_f((int)f.idx);
			uint64_t bits;
			std::memcpy(&bits, &d, sizeof(double));
			if (f.value.size() <= 10) bits &= 0xFFFFFFFFull;   // a 32-bit FLEN
			if (bits != want) {
				fail("f" + std::to_string(f.idx) + ": reference " + f.value + ", DoomV " + hex(bits, 16));
				return;
			}
		} else if (f.cls == 'v') {
			if (f.idx > 31) continue;
			const std::vector<uint8_t> want_bytes = hex_bytes(f.value);
			const uint8_t *v = regs.read_v((int)f.idx);
			if (want_bytes.size() != (size_t)VB || std::memcmp(v, want_bytes.data(), VB) != 0) {
				fail("v" + std::to_string(f.idx) + " differs (or the reference's VLEN is not "
				     + std::to_string(Registers::VLEN_BITS) + ")");
				return;
			}
		} else if (f.cls == 'c') {
			if (from_ref_csr((uint16_t)f.idx)) continue;
			const uint64_t mine = step_csr((uint16_t)f.idx);
			if (mine != want) {
				fail("CSR " + std::string(csr_label((uint16_t)f.idx)) + " (" + hex(f.idx, 3) + "): reference "
				     + hex(want, 16) + ", DoomV " + hex(mine, 16));
				return;
			}
		}
	}

	// Stores: every one the reference made, same address and value, and none it
	// did not. Sail names physical addresses; Spike virtual ones.
	const auto same_addr = [&](const AccessRecord &a, uint64_t addr) { return (ref.physical ? a.paddr : a.vaddr) == addr; };
	for (const RefStore &s : ref.stores) {
		const size_t size = hex_bytes(s.value).size();
		uint64_t want = 0;
		parse_hex(s.value, want);
		bool found = false;
		uint64_t paddr = 0;
		for (const AccessRecord &a : writes) if (same_addr(a, s.addr)) { found = true; paddr = a.paddr; }
		if (!found) {
			// Vector and hypervisor loads and stores translate on their own,
			// outside translate_or_trap, so they are not in the access log.
			if (ref.physical) {
				paddr = s.addr;
			} else {
				uint64_t cause = 0, tval = 0;
				if (!mmu_translate(regs, memory, s.addr, AccessType::Store, paddr, cause, tval)) {
					fail("the reference stored to " + hex(s.addr, 16) + " and DoomV did not");
					return;
				}
			}
		}
		uint64_t mine = 0;
		if (!store_value(paddr, (unsigned)size, mine)) {
			if (!memory.is_ram(paddr, (unsigned)size)) {
				fail("the reference stored to " + hex(s.addr, 16) + " and DoomV did not");
				return;
			}
			uint8_t bytes[8] = {};
			memory.read_bytes(paddr, bytes, std::min(size, (size_t)8));
			for (size_t i = 0; i < std::min(size, (size_t)8); i++) mine |= (uint64_t)bytes[i] << (8 * i);
		}
		if (mine != want) {
			fail("store to " + hex(s.addr, 16) + ": reference " + s.value + ", DoomV " + hex(mine, (int)size * 2));
			return;
		}
	}
	// Sail zeroes a cbo.zero block without passing through its memory trace,
	// so its trace lists no stores for one; the block itself is checked by
	// every later load of it.
	const bool cbo_zero = step_insn_len == 4 && step_insn == ((step_insn & 0x000F8000u) | 0x0040200Fu);
	for (const AccessRecord &a : writes) {
		if (cbo_zero && ref.stores.empty()) break;
		bool listed = false;
		for (const RefStore &s : ref.stores) if (same_addr(a, s.addr)) listed = true;
		if (!listed) {
			fail("DoomV stored to " + hex(ref.physical ? a.paddr : a.vaddr, 16) + ", which the reference did not");
			return;
		}
	}

	lock->matched++;
}
