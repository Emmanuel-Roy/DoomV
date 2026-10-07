// A testbench's side of the in-process lock-step (src/doomv_lockstep.h): it
// reads a Sail trace and hands its records to DoomV, the way a core's C
// simulation or RTL testbench hands over what the core retired. Built with
// Vitis HLS's own MinGW g++ when there is one -- the compiler a Vitis
// testbench is built with -- by tools/verification/lockstep_lib.py.
//
//   feed text   <trace> <DoomV arguments...>   records as Sail's text, one at a time
//   feed struct <trace> <DoomV arguments...>   records built as doomv_ls_record
//
// The struct form parses the trace itself (sail_records.hpp), independently
// of DoomV's reader: what a testbench builds from its core's retirement port. Exit status 0
// when every record matched, 1 at a mismatch (printed), 2 for anything else.
#include "doomv_lockstep.h"
#include "sail_records.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace sail_records;

int main(int argc, char **argv)
{
	if (argc < 4 || (std::strcmp(argv[1], "text") != 0 && std::strcmp(argv[1], "struct") != 0)) {
		std::fprintf(stderr, "usage: feed text|struct <trace> <DoomV arguments...>\n");
		return 2;
	}
	if (doomv_ls_version() != DOOMV_LS_VERSION) {
		std::fprintf(stderr, "feed: the DLL is interface version %d, this header %d\n", doomv_ls_version(), DOOMV_LS_VERSION);
		return 2;
	}
	const bool text = std::strcmp(argv[1], "text") == 0;
	std::ifstream in(argv[2], std::ios::binary);
	if (!in) { std::fprintf(stderr, "feed: cannot read %s\n", argv[2]); return 2; }
	doomv_ls *ls = doomv_ls_open(argc - 3, argv + 3);
	if (!ls) { std::fprintf(stderr, "feed: %s\n", doomv_ls_open_error()); return 2; }

	int status = DOOMV_LS_MATCH;
	uint64_t calls = 0;
	if (text) {
		// A record at a time where the trace stamps them; otherwise -- several
		// harts, whose records can be split by each other's lines -- whole.
		std::string chunk, line;
		bool stamped = false;
		std::vector<std::string> chunks;
		while (std::getline(in, line)) {
			if (starts(line, "cycle ")) {
				stamped = true;
				if (!chunk.empty()) chunks.push_back(chunk);
				chunk.clear();
			}
			chunk += line;
			chunk += '\n';
		}
		chunks.push_back(chunk);
		(void)stamped;
		for (const std::string &c : chunks) {
			calls++;
			status = doomv_ls_step_text(ls, c.c_str());
			if (status != DOOMV_LS_MATCH) break;
		}
	} else {
		std::vector<Record> records;
		if (!parse_records(in, records)) { std::fprintf(stderr, "feed: cannot read the trace's records\n"); return 2; }
		for (Record &rec : records) {
			calls++;
			status = doomv_ls_step(ls, &rec.finish());
			if (status != DOOMV_LS_MATCH) break;
		}
	}
	const uint64_t matched = doomv_ls_matched(ls);
	if (status == DOOMV_LS_MATCH) {
		std::printf("feed: %llu records matched in %llu calls (%llu values taken)\n", (unsigned long long)matched,
		            (unsigned long long)calls, (unsigned long long)doomv_ls_taken(ls));
	} else {
		std::printf("feed: status %d after %llu matching records\n%s\n", status, (unsigned long long)matched,
		            doomv_ls_message(ls));
	}
	std::fflush(stdout);
	doomv_ls_close(ls);
	return status == DOOMV_LS_MATCH ? 0 : status == DOOMV_LS_MISMATCH ? 1 : 2;
}
