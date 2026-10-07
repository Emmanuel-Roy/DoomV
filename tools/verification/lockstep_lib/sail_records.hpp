// Sail's trace read as doomv_ls_records (src/doomv_lockstep.h): what a
// testbench builds from its core's retirement port, here built from the
// golden reference's trace instead. A trace with a "cycle <n>" line before
// every record, one hart, as DoomV's -lockstep-stamp writes it. Used by
// feed.cpp and by the Vitis testbench (vitis/retire_tb.cpp).
#pragma once
#include "doomv_lockstep.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <iterator>
#include <string>
#include <vector>

namespace sail_records {

struct Named { uint64_t cause; const char *name; };
// Sail's names (model/core/types.sail), as its trace prints them.
static const Named exceptions[] = {
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
static const Named interrupts[] = {
	{1, "supervisor-software-interrupt"}, {2, "virtual-supervisor-software-interrupt"},
	{3, "machine-software-interrupt"}, {5, "supervisor-timer-interrupt"},
	{6, "virtual-supervisor-timer-interrupt"}, {7, "machine-timer-interrupt"},
	{9, "supervisor-external-interrupt"}, {10, "virtual-supervisor-external-interrupt"},
	{11, "machine-external-interrupt"}, {12, "supervisor guest-external-interrupt"},
	{13, "counter-overflow interrupt"},
};

inline bool starts(const std::string &s, const char *p) { return s.compare(0, std::strlen(p), p) == 0; }

inline uint64_t hex(const std::string &s) { return std::strtoull(s.c_str(), nullptr, 16); }

// "0x..." to bytes, least significant first.
inline std::vector<uint8_t> hex_bytes(std::string s)
{
	if (starts(s, "0x")) s = s.substr(2);
	if (s.size() % 2) s.insert(s.begin(), '0');
	std::vector<uint8_t> out;
	for (size_t i = s.size(); i >= 2; i -= 2) out.push_back((uint8_t)std::strtoul(s.substr(i - 2, 2).c_str(), nullptr, 16));
	return out;
}

// One record, with the storage its doomv_ls_record points into.
struct Record {
	doomv_ls_record r{};
	std::vector<doomv_ls_write> writes, trap_writes;
	std::vector<doomv_ls_store> stores;
	std::vector<std::vector<uint8_t>> bytes;   // vector registers and wide stores
	bool trap = false;

	const doomv_ls_record &finish()
	{
		// Pointers last: the vectors are complete.
		for (doomv_ls_write &w : writes) if (w.kind == 'v') w.bytes = bytes[(size_t)w.value].data();
		for (doomv_ls_store &s : stores) if (s.size > 8) s.bytes = bytes[(size_t)s.value].data();
		r.writes = writes.data();
		r.n_writes = (uint32_t)writes.size();
		r.trap_writes = trap_writes.data();
		r.n_trap_writes = (uint32_t)trap_writes.size();
		r.stores = stores.data();
		r.n_stores = (uint32_t)stores.size();
		return r;
	}
};

inline bool parse_priv(const std::string &p, uint32_t &priv, bool &virt)
{
	virt = p[0] == 'V';
	const std::string m = virt ? p.substr(1) : p;
	if (m == "M") priv = 3;
	else if (m == "S" || m == "HS") priv = 1;
	else if (m == "U") priv = 0;
	else return false;
	return true;
}

// A Sail trace with a "cycle <n>" line before every record (one hart), as
// doomv_ls_records. Returns false if a line cannot be read.
inline bool parse_records(std::istream &in, std::vector<Record> &out)
{
	std::string line;
	bool open = false;
	while (std::getline(in, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (starts(line, "cycle ")) {
			out.emplace_back();
			Record &n = out.back();
			n.r.flags = DOOMV_LS_HAS_CYCLE;
			n.r.cycle = std::strtoull(line.c_str() + 6, nullptr, 0);
			open = true;
			continue;
		}
		if (!open) continue;
		Record &rec = out.back();
		doomv_ls_record &r = rec.r;
		if (line[0] == '[') {
			// [N] [P]: 0xPC (0xINSN) ...
			const size_t p1 = line.find("] ["), p2 = line.find("]: 0x"), open_paren = line.find(" (0x");
			bool virt = false;
			if (p1 == std::string::npos || p2 == std::string::npos || open_paren == std::string::npos
			    || !parse_priv(line.substr(p1 + 3, p2 - p1 - 3), r.priv, virt))
				return false;
			const size_t close = line.find(')', open_paren);
			const std::string insn = line.substr(open_paren + 2, close - open_paren - 2);
			r.flags |= DOOMV_LS_HAS_INSN | (virt ? DOOMV_LS_VIRT : 0);
			r.pc = hex(line.substr(p2 + 3, open_paren - p2 - 3));
			r.insn = (uint32_t)hex(insn);
			r.insn_len = insn.size() - 2 <= 4 ? 2 : 4;
		} else if (starts(line, "handling exc#") || starts(line, "handling int#")) {
			const bool irq = starts(line, "handling int#");
			const std::string name = line.substr(13, line.find(" at priv ") - 13);
			bool known = false;
			for (const Named &n : irq ? std::vector<Named>(std::begin(interrupts), std::end(interrupts))
			                          : std::vector<Named>(std::begin(exceptions), std::end(exceptions)))
				if (name == n.name) { r.cause = n.cause; known = true; }
			if (!known) return false;
			r.kind = irq ? DOOMV_LS_INTERRUPT : DOOMV_LS_EXCEPTION;
			const size_t t = line.find("| tval=");
			if (t != std::string::npos) {
				r.tval = hex(line.substr(t + 7, line.find(' ', t + 7) - t - 7));
				r.flags |= DOOMV_LS_HAS_TVAL;
			}
			rec.trap = true;
		} else if (starts(line, "CSR ")) {
			const size_t o = line.find(" (0x"), a = line.find(") <- ");
			if (o == std::string::npos || a == std::string::npos) continue;   // a read
			doomv_ls_write w{};
			w.kind = 'c';
			w.index = (uint16_t)hex(line.substr(o + 2, a - o - 2));
			w.value = hex(line.substr(a + 5));
			if (rec.trap) {
				rec.trap_writes.push_back(w);
				if (w.index == 0x341 || w.index == 0x141 || w.index == 0x241) { r.epc = w.value; r.flags |= DOOMV_LS_HAS_EPC; }
			} else {
				rec.writes.push_back(w);
			}
		} else if (starts(line, "mem[W,") || starts(line, "mem[RW,") || starts(line, "mem[C,")) {
			if (rec.trap) continue;
			const size_t comma = line.find(','), close = line.find("] <- ");
			doomv_ls_store s{};
			s.paddr = hex(line.substr(comma + 1, close - comma - 1));
			std::vector<uint8_t> b = hex_bytes(line.substr(close + 5));
			s.size = (uint32_t)b.size();
			if (s.size <= 8) {
				for (size_t i = 0; i < b.size(); i++) s.value |= (uint64_t)b[i] << (8 * i);
			} else {
				s.value = rec.bytes.size();   // an index, until finish()
				rec.bytes.push_back(b);
			}
			rec.stores.push_back(s);
		} else {
			const size_t a = line.find(" <- 0x");
			if (a == std::string::npos || rec.trap) continue;
			const char cls = line[0];
			if (cls != 'x' && cls != 'f' && cls != 'v') continue;
			doomv_ls_write w{};
			w.kind = cls;
			w.index = (uint16_t)std::atoi(line.c_str() + 1);
			if (cls == 'v') {
				w.value = rec.bytes.size();   // an index, until finish()
				rec.bytes.push_back(hex_bytes(line.substr(a + 4)));
			} else {
				w.value = hex(line.substr(a + 4));
			}
			rec.writes.push_back(w);
		}
	}
	// One record, two steps: an instruction, then a fetch fault at the pc
	// after it -- what a core retires as two.
	std::vector<Record> split;
	for (Record &rec : out) {
		doomv_ls_record &r = rec.r;
		const bool fetch = r.cause == 1 || r.cause == 12 || r.cause == 20;
		if (r.kind == DOOMV_LS_EXCEPTION && (r.flags & DOOMV_LS_HAS_INSN) && fetch
		    && (r.flags & DOOMV_LS_HAS_EPC) && r.epc != r.pc) {
			Record first = rec;
			first.r.kind = DOOMV_LS_COMMIT;
			first.r.flags &= ~(uint32_t)(DOOMV_LS_HAS_EPC | DOOMV_LS_HAS_TVAL);
			first.trap_writes.clear();
			Record second = rec;
			second.r.flags &= ~(uint32_t)DOOMV_LS_HAS_INSN;
			second.writes.clear();
			second.stores.clear();
			split.push_back(first);
			split.push_back(second);
		} else {
			split.push_back(rec);
		}
	}
	out.swap(split);
	return true;
}

} // namespace sail_records
