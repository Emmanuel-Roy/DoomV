#pragma once
#include <cstdint>

// One counter for "something that could change whether an interrupt is due has
// changed", bumped alongside every generation the interrupt check used to add
// up: Registers::state_gen (privilege, V, any CSR write), each IMSIC file's
// gen, the timer's mtimecmp gen, and ExtensionsEpoch.
//
// That check runs on every single step, and summing four counters meant four
// loads from four objects on four different cache lines to conclude, almost
// always, that nothing had changed. One counter makes it one load. Each source
// keeps its own generation as well -- other code reads those, and the decode
// cache is keyed on ExtensionsEpoch specifically, not on this.
//
// The invariant: every write that bumps one of those generations bumps this
// one too, in the same statement. Miss one and an interrupt goes unnoticed
// until something unrelated moves the counter -- a divergence lock-step
// against Sail catches, but only on a workload that happens to hit it. Hence
// bump_event_gen() rather than a bare `EventGen++` at each site: it is the
// thing to grep for when adding a new generation.
inline uint64_t EventGen = 0;
inline void bump_event_gen() { EventGen++; }
