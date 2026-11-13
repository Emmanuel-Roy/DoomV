// Per-function retired-instruction profile: the lock-step drill-down.
//
// When DoomV and a real Pico retire different numbers of instructions in a
// frame, this says *which functions* differ. Built only with
// -DPORT_FUNCPROF=ON, identically into the firmware and the DoomV guest:
// the engine is compiled with -finstrument-functions, so every engine
// function calls the hooks below on entry and exit, and each stretch of
// instructions between two hooks is charged to the function running.
//
// The hooks must themselves retire the same instructions on both machines,
// or they would perturb what they measure -- and function addresses differ
// between the two images. So they are branch-free and constant-cost: a
// direct-mapped table indexed by a hash of the address (a collision merges
// two functions' counts instead of taking a different path), a fixed-size
// stack indexed by a mask, and the hook's own instructions excluded from
// what is charged. Platform code already set aside at run time (inside
// DG_GetTicksMs/DG_GetKey) is subtracted too.
//
// A game-logic window over PROF_DUMP_OVER instructions (a level load, say)
// is dumped as "funcprof <frame> <address> <instructions>" per function;
// tools/funcprof_diff.py maps addresses to names with each ELF's symbols.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port.h"
#include "port_instret.h"
#include "port_link.h"

#define NOPROF __attribute__((no_instrument_function))

#ifndef PROF_BITS
#define PROF_BITS 10                      // 1024 slots, 9 KB: more would cost E1M5 its place in RAM
#endif
#define PROF_SLOTS (1u << PROF_BITS)
#define PROF_DEPTH 256                    // power of two
#define PROF_DUMP_OVER 300000u

static uintptr_t slot_fn[PROF_SLOTS];
static uint32_t slot_count[PROF_SLOTS];
static uint8_t slot_merged[PROF_SLOTS];  // two functions shared the slot: not comparable
static uint32_t stack[PROF_DEPTH];
static uint32_t depth, cur, last, last_excl;
static int active;                        // only inside counted game-logic windows

static NOPROF inline uint32_t slot_of(void *fn)
{
	return (uint32_t)(((uintptr_t)fn >> 1) * 2654435761u) >> (32 - PROF_BITS);
}

// Charge everything since the last hook (minus excluded platform code) to
// the running function. Branch-free.
static NOPROF inline void charge(void)
{
	uint32_t now = port_instret(), excl = port_link_excluded_instret();
	slot_count[cur] += (now - last) - (excl - last_excl);
	last_excl = excl;
}

static NOPROF inline void restart(void) { last = port_instret(); }

NOPROF void __cyg_profile_func_enter(void *fn, void *site)
{
	(void)site;
	if (!active) return;                  // same outcome on both machines inside a window
	charge();
	stack[depth & (PROF_DEPTH - 1)] = cur;
	depth++;
	cur = slot_of(fn);
	uintptr_t empty = (uintptr_t)0 - (uintptr_t)(slot_fn[cur] == 0);   // all ones if the slot is free
	slot_fn[cur] |= (uintptr_t)fn & empty;
	slot_merged[cur] |= (uint8_t)(slot_fn[cur] != (uintptr_t)fn);
	restart();
}

NOPROF void __cyg_profile_func_exit(void *fn, void *site)
{
	(void)fn; (void)site;
	if (!active) return;
	charge();
	depth--;
	cur = stack[depth & (PROF_DEPTH - 1)];
	restart();
}

// In the recording (TEXT records) on the Pico, on the console elsewhere.
static NOPROF void prof_line(const char *line) { port_text(line); }

// port_perf.c brackets each game-logic window with these.
NOPROF void port_funcprof_begin(void)
{
	depth = 0;
	cur = 0;                              // slot 0 collects anything outside a function
	last_excl = port_link_excluded_instret();
	active = 1;
	restart();
}

NOPROF void port_funcprof_end(uint32_t frame, uint32_t window_instret)
{
	active = 0;
	if (window_instret > PROF_DUMP_OVER) {
		char line[64];
		for (uint32_t i = 0; i < PROF_SLOTS; i++) {
			if (!slot_count[i]) continue;
			snprintf(line, sizeof line, "funcprof %lu %08lx %lu%s", (unsigned long)frame,
			         (unsigned long)slot_fn[i], (unsigned long)slot_count[i], slot_merged[i] ? " merged" : "");
			prof_line(line);
		}
	}
	memset(slot_fn, 0, sizeof slot_fn);
	memset(slot_count, 0, sizeof slot_count);
	memset(slot_merged, 0, sizeof slot_merged);
}
