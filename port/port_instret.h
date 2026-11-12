// Retired-instruction counter, for comparing execution at retirement.
//
// Hazard3 (like any pipelined core) has instructions in flight that may
// never complete -- squashed by a branch, a trap, a stall. minstret counts
// only instructions that *retired*: the architectural progress of the hart,
// which is what DoomV (an instruction-at-a-time model) can be held to.
//
// Inline on purpose: the read is one `csrr` in the caller, so a measured
// window contains only shared code -- identical instructions on the Pico and
// on DoomV. Only the low 32 bits are read; windows are a frame long, far
// below 2^32 instructions, and unsigned subtraction handles the wrap.
#pragma once
#include <stdint.h>

static inline uint32_t port_instret(void)
{
#if defined(__riscv)
	uint32_t v;
	__asm__ volatile("csrr %0, minstret" : "=r"(v));
	return v;
#else
	return 0;   // the PC sim has no such counter; its counts read as 0
#endif
}
