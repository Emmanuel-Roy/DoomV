#pragma once
// A reference's record of one step, as lockstep.cpp compares it: read from a
// trace (Sail's format, or Spike's), or handed over in-process through
// doomv_lockstep.h (lockstep_api.cpp).
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lockstep {

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
	// The core's cycle count at this step, from a "cycle <n>" line before
	// the record or the in-process record: the clock with -cycle-clock.
	bool has_cycle = false;
	uint64_t cycle = 0;
	// The first of two steps one trace record holds (an instruction, then a
	// fetch fault at the pc after it), so the hart steps again at once.
	bool continues = false;
	bool split_tail = false;   // and the second
};

} // namespace lockstep
