// The testbench: the stamped Sail trace DOOMV_LS_TRACE names, as records, in
// at retire_port's input; what comes out -- the C++ in C simulation, the RTL
// in co-simulation -- back into records, each handed to DoomV
// (doomv_lockstep.h) as it would be from a core's retirement port, on the
// machine DOOMV_LS_ARGS describes. 0 when every record matches.
#include "doomv_lockstep.h"
#include "retire_port.hpp"
#include "sail_records.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

using namespace sail_records;

namespace {

void put_bytes(std::vector<uint64_t> &w, const std::vector<uint8_t> &b)
{
	for (size_t i = 0; i < b.size(); i += 8) {
		uint64_t v = 0;
		for (size_t k = 0; k < 8 && i + k < b.size(); k++) v |= (uint64_t)b[i + k] << (8 * k);
		w.push_back(v);
	}
}

std::vector<uint8_t> get_bytes(const std::vector<uint64_t> &w, size_t &at, size_t n)
{
	std::vector<uint8_t> b(n);
	for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(w[at + i / 8] >> (8 * (i % 8)));
	at += (n + 7) / 8;
	return b;
}

// A record as the words a retirement port would carry.
void put(std::vector<uint64_t> &w, const Record &rec, unsigned vbytes)
{
	const doomv_ls_record &r = rec.r;
	w.push_back((uint64_t)r.hart | (uint64_t)r.kind << 32 | (uint64_t)r.flags << 40 | (uint64_t)r.priv << 56);
	w.push_back(r.cycle);
	w.push_back(r.pc);
	w.push_back((uint64_t)r.insn | (uint64_t)r.insn_len << 32);
	w.push_back(r.cause);
	w.push_back(r.epc);
	w.push_back(r.tval);
	w.push_back((uint64_t)rec.writes.size() | (uint64_t)rec.trap_writes.size() << 16 | (uint64_t)rec.stores.size() << 32);
	for (const auto *list : {&rec.writes, &rec.trap_writes})
		for (const doomv_ls_write &x : *list) {
			w.push_back((uint64_t)(uint8_t)x.kind | (uint64_t)x.index << 8);
			if (x.kind == 'v') put_bytes(w, rec.bytes[(size_t)x.value]);
			else w.push_back(x.value);
		}
	for (const doomv_ls_store &s : rec.stores) {
		w.push_back(s.paddr);
		w.push_back(s.size);
		if (s.size > 8) put_bytes(w, rec.bytes[(size_t)s.value]);
		else w.push_back(s.value);
	}
	(void)vbytes;
}

Record get(const std::vector<uint64_t> &w, size_t &at, unsigned vbytes)
{
	Record rec;
	doomv_ls_record &r = rec.r;
	const uint64_t h = w[at++];
	r.hart = (uint32_t)h;
	r.kind = (uint32_t)(h >> 32) & 0xFF;
	r.flags = (uint32_t)(h >> 40) & 0xFFFF;
	r.priv = (uint32_t)(h >> 56);
	r.cycle = w[at++];
	r.pc = w[at++];
	const uint64_t insn = w[at++];
	r.insn = (uint32_t)insn;
	r.insn_len = (uint32_t)(insn >> 32);
	r.cause = w[at++];
	r.epc = w[at++];
	r.tval = w[at++];
	const uint64_t n = w[at++];
	const size_t nw = n & 0xFFFF, nt = (n >> 16) & 0xFFFF, ns = n >> 32;
	for (size_t i = 0; i < nw + nt; i++) {
		doomv_ls_write x{};
		const uint64_t k = w[at++];
		x.kind = (char)(k & 0xFF);
		x.index = (uint16_t)(k >> 8);
		if (x.kind == 'v') {
			x.value = rec.bytes.size();
			rec.bytes.push_back(get_bytes(w, at, vbytes));
		} else {
			x.value = w[at++];
		}
		(i < nw ? rec.writes : rec.trap_writes).push_back(x);
	}
	for (size_t i = 0; i < ns; i++) {
		doomv_ls_store s{};
		s.paddr = w[at++];
		s.size = (uint32_t)w[at++];
		if (s.size > 8) {
			s.value = rec.bytes.size();
			rec.bytes.push_back(get_bytes(w, at, s.size));
		} else {
			s.value = w[at++];
		}
		rec.stores.push_back(s);
	}
	return rec;
}

} // namespace

int main()
{
	const char *trace = std::getenv("DOOMV_LS_TRACE");
	const char *args = std::getenv("DOOMV_LS_ARGS");
	if (!trace || !args) {
		std::printf("retire_tb: set DOOMV_LS_TRACE (a stamped Sail trace) and DOOMV_LS_ARGS (DoomV's arguments)\n");
		return 2;
	}
	const char *vlen_env = std::getenv("DOOMV_LS_VLEN");
	const unsigned vbytes = (vlen_env ? (unsigned)std::atoi(vlen_env) : 128) / 8;
	std::ifstream f(trace, std::ios::binary);
	std::vector<Record> records;
	if (!f || !parse_records(f, records)) {
		std::printf("retire_tb: cannot read %s\n", trace);
		return 2;
	}

	// Through the retirement port.
	std::vector<uint64_t> words;
	for (const Record &rec : records) put(words, rec, vbytes);
	hls::stream<retire_word> in("in"), out("out");
	for (uint64_t w : words) in.write(w);
	retire_port(in, out, (int)words.size());
	std::vector<uint64_t> back;
	while (!out.empty()) back.push_back((uint64_t)out.read());
	if (back != words) {
		std::printf("retire_tb: the port gave back %zu words, not the %zu it was given\n", back.size(), words.size());
		return 2;
	}

	doomv_ls *ls = doomv_ls_open_line(args);
	if (!ls) {
		std::printf("retire_tb: %s\n", doomv_ls_open_error());
		return 2;
	}
	size_t at = 0;
	int status = DOOMV_LS_MATCH;
	while (at < back.size() && status == DOOMV_LS_MATCH) {
		Record rec = get(back, at, vbytes);
		status = doomv_ls_step(ls, &rec.finish());
	}
	if (status == DOOMV_LS_MATCH)
		std::printf("retire_tb: %llu records from the retirement port matched DoomV\n",
		            (unsigned long long)doomv_ls_matched(ls));
	else
		std::printf("retire_tb: status %d\n%s\n", status, doomv_ls_message(ls));
	std::fflush(stdout);
	doomv_ls_close(ls);
	return status == DOOMV_LS_MATCH ? 0 : 1;
}
