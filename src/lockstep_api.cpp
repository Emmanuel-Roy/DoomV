// doomv_lockstep.h: DoomV as a library, for a core's simulation to lock-step
// against in its own process. The comparison is lockstep.cpp's; this turns
// the C interface's records into its RefRecords.

#include "doomv_lockstep.h"
#include "doom_system.hpp"
#include "lockstep_record.hpp"
#include "machine.hpp"
#include <cinttypes>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

struct doomv_ls {
	std::unique_ptr<DoomSystem> system;
	std::string message;
};

namespace {

std::string open_error;
bool opened = false;   // one machine per process: see machine.hpp

std::string hex_value(uint64_t v, unsigned bytes)
{
	char buf[24];
	std::snprintf(buf, sizeof buf, "0x%0*" PRIX64, (int)(bytes * 2), v);
	return buf;
}

// Bytes, the lowest address first, as one hex number.
std::string hex_bytes(const uint8_t *b, size_t n)
{
	static const char digits[] = "0123456789ABCDEF";
	std::string s = "0x";
	s.reserve(2 + n * 2);
	for (size_t i = n; i-- > 0;) {
		s += digits[b[i] >> 4];
		s += digits[b[i] & 15];
	}
	return s;
}

bool convert_write(const doomv_ls_write &w, lockstep::RefField &f, std::string &error)
{
	f.cls = w.kind;
	f.idx = w.index;
	switch (w.kind) {
	case 'x': case 'f': case 'c':
		f.value = hex_value(w.value, 8);
		return true;
	case 'v':
		if (!w.bytes) { error = "a vector register write with no bytes"; return false; }
		f.value = hex_bytes(w.bytes, Registers::VLEN_BYTES);
		return true;
	default:
		error = std::string("a write of kind '") + w.kind + "': x, f, v or c";
		return false;
	}
}

} // namespace

extern "C" {

int doomv_ls_version(void) { return DOOMV_LS_VERSION; }

const char *doomv_ls_open_error(void) { return open_error.c_str(); }

doomv_ls *doomv_ls_open(int argc, const char *const *argv)
{
	open_error.clear();
	if (opened) {
		open_error = "one machine per process: the extensions, VLEN and RAM size it sets are the process's";
		return nullptr;
	}
	std::vector<const char *> args{"doomv"};
	for (int i = 0; i < argc; i++) args.push_back(argv[i]);
	auto ls = std::make_unique<doomv_ls>();
	// What setting up says goes to the caller's output as ever, and is kept
	// for doomv_ls_open_error.
	std::ostringstream said;
	std::streambuf *const out = std::cout.rdbuf(said.rdbuf());
	int status = 0;
	bool ok = false;
	try {
		ok = setup_machine((int)args.size(), args.data(), ls->system, status, true);
	} catch (const std::exception &e) {
		said << e.what() << "\n";
	}
	std::cout.rdbuf(out);
	std::cout << said.str() << std::flush;
	if (!ok) {
		open_error = said.str().empty() ? "the arguments set up no machine to step" : said.str();
		return nullptr;
	}
	opened = true;
	return ls.release();
}

doomv_ls *doomv_ls_open_line(const char *arguments)
{
	std::vector<std::string> words;
	std::string w;
	bool quoted = false, any = false;
	for (const char *p = arguments ? arguments : ""; *p; p++) {
		if (*p == '"') { quoted = !quoted; any = true; continue; }
		if (!quoted && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
			if (any) words.push_back(w);
			w.clear();
			any = false;
			continue;
		}
		w += *p;
		any = true;
	}
	if (any) words.push_back(w);
	std::vector<const char *> argv;
	for (const std::string &s : words) argv.push_back(s.c_str());
	return doomv_ls_open((int)argv.size(), argv.data());
}

int doomv_ls_step(doomv_ls *ls, const doomv_ls_record *rec)
{
	lockstep::RefRecord r;
	r.physical = true;
	r.kind = rec->kind == DOOMV_LS_EXCEPTION ? lockstep::RefRecord::Exception
	       : rec->kind == DOOMV_LS_INTERRUPT ? lockstep::RefRecord::Interrupt
	                                         : lockstep::RefRecord::Commit;
	r.priv = (int)rec->priv;
	r.virt = (rec->flags & DOOMV_LS_VIRT) != 0;
	r.has_insn = (rec->flags & DOOMV_LS_HAS_INSN) != 0;
	r.pc = rec->pc;
	r.insn = rec->insn;
	r.insn_digits = rec->insn_len == 2 ? 4 : 8;
	r.has_cycle = (rec->flags & DOOMV_LS_HAS_CYCLE) != 0;
	r.cycle = rec->cycle;
	r.cause = rec->cause;
	r.has_epc = (rec->flags & DOOMV_LS_HAS_EPC) != 0;
	r.epc = rec->epc;
	r.has_tval = (rec->flags & DOOMV_LS_HAS_TVAL) != 0;
	r.tval = rec->tval;
	r.trap_started = r.kind != lockstep::RefRecord::Commit;
	std::string error;
	r.fields.resize(rec->n_writes);
	for (uint32_t i = 0; i < rec->n_writes; i++)
		if (!convert_write(rec->writes[i], r.fields[i], error)) { ls->message = "lockstep: " + error; return DOOMV_LS_ERROR; }
	r.trap_fields.resize(rec->n_trap_writes);
	for (uint32_t i = 0; i < rec->n_trap_writes; i++)
		if (!convert_write(rec->trap_writes[i], r.trap_fields[i], error)) { ls->message = "lockstep: " + error; return DOOMV_LS_ERROR; }
	r.stores.resize(rec->n_stores);
	for (uint32_t i = 0; i < rec->n_stores; i++) {
		const doomv_ls_store &s = rec->stores[i];
		r.stores[i].addr = s.paddr;
		if (s.size > 8 && !s.bytes) { ls->message = "lockstep: a store of more than 8 bytes with no bytes"; return DOOMV_LS_ERROR; }
		r.stores[i].value = s.size > 8 ? hex_bytes(s.bytes, s.size) : hex_value(s.value & (s.size == 8 ? ~0ull : (1ull << (8 * s.size)) - 1), s.size);
	}
	return ls->system->lockstep_step_record(std::move(r), rec->hart, ls->message);
}

int doomv_ls_step_text(doomv_ls *ls, const char *records)
{
	return ls->system->lockstep_step_text(records ? records : "", ls->message);
}

const char *doomv_ls_message(const doomv_ls *ls) { return ls->message.c_str(); }
uint64_t doomv_ls_matched(const doomv_ls *ls) { return ls->system->lockstep_matched(); }
uint64_t doomv_ls_taken(const doomv_ls *ls) { return ls->system->lockstep_taken(); }
uint64_t doomv_ls_steps(const doomv_ls *ls) { return ls->system->steps_taken(); }
uint64_t doomv_ls_pc(const doomv_ls *ls, uint32_t hart)
{
	return hart < ls->system->hart_count() ? ls->system->hart_pc(hart) : 0;
}
uint64_t doomv_ls_tohost(const doomv_ls *ls) { return ls->system->tohost_value(); }

int doomv_ls_snapshot(doomv_ls *ls, const char *dir) { return ls->system->save_snapshot(dir) ? 0 : 1; }
int doomv_ls_export_state(doomv_ls *ls, const char *dir) { return ls->system->export_state(dir) ? 0 : 1; }

void doomv_ls_close(doomv_ls *ls)
{
	if (!ls) return;
	std::cout << std::flush;
	delete ls;
}

} // extern "C"
