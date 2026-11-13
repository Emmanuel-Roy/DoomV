// minstret self-test: what a `csrr minstret` sees, around known sequences.
//
// Lock-step compares counts measured as (minstret after) - (minstret before).
// On a pipelined core the question is exactly which instructions a read
// includes -- does a read right after a load, a load-use stall, a divide or
// a taken branch see that instruction retired? This runs the same short
// sequences on every RISC-V target and logs each delta; the Pico's and
// DoomV's must agree line for line. Shared code, so both run identical
// instructions. Run once at start-up (port_run).

#include <stdint.h>
#include <stdio.h>

#include "port.h"
#include "port_link.h"

#define NOINL __attribute__((noinline))

static volatile uint32_t mem_word = 7;

static NOINL uint32_t leaf(uint32_t x) { return x + 1; }

void port_instret_selftest(void)
{
#if defined(__riscv)
	uint32_t a, b, t;
	char line[96];
	struct { const char *what; uint32_t delta, expect; } r[8];
	int n = 0;

#define READ(v) __asm__ volatile("csrr %0, minstret" : "=r"(v))
	// 1. Back to back: the second read sees the first retired.
	__asm__ volatile("csrr %0, minstret\n\tcsrr %1, minstret" : "=&r"(a), "=r"(b));
	r[n].what = "csrr; csrr"; r[n].delta = b - a; r[n++].expect = 1;
	// 2. A load in between.
	__asm__ volatile("csrr %0, minstret\n\tlw %2, 0(%3)\n\tcsrr %1, minstret"
	                 : "=&r"(a), "=r"(b), "=&r"(t) : "r"(&mem_word));
	r[n].what = "csrr; lw; csrr"; r[n].delta = b - a; r[n++].expect = 2;
	// 3. A load, then an instruction that uses it (load-use stall).
	__asm__ volatile("csrr %0, minstret\n\tlw %2, 0(%3)\n\taddi %2, %2, 1\n\tcsrr %1, minstret"
	                 : "=&r"(a), "=r"(b), "=&r"(t) : "r"(&mem_word));
	r[n].what = "csrr; lw; addi(use); csrr"; r[n].delta = b - a; r[n++].expect = 3;
	// 4. A divide (many cycles on Hazard3).
	__asm__ volatile("csrr %0, minstret\n\tdiv %2, %3, %4\n\tcsrr %1, minstret"
	                 : "=&r"(a), "=r"(b), "=&r"(t) : "r"(1000000), "r"(7));
	r[n].what = "csrr; div; csrr"; r[n].delta = b - a; r[n++].expect = 2;
	// 5. A taken branch.
	__asm__ volatile("csrr %0, minstret\n\tbeq zero, zero, 1f\n\tnop\n1:\tcsrr %1, minstret"
	                 : "=&r"(a), "=r"(b));
	r[n].what = "csrr; beq(taken); csrr"; r[n].delta = b - a; r[n++].expect = 2;
	// 6. A store.
	__asm__ volatile("csrr %0, minstret\n\tsw %2, 0(%3)\n\tcsrr %1, minstret"
	                 : "=&r"(a), "=r"(b) : "r"(5), "r"(&mem_word) : "memory");
	r[n].what = "csrr; sw; csrr"; r[n].delta = b - a; r[n++].expect = 2;
	// 7. A call and return (C: count depends on the compiler, but must agree).
	READ(a); t = leaf(a); READ(b);
	r[n].what = "csrr; call leaf; csrr"; r[n].delta = b - a; r[n++].expect = 0;
	(void)t;
#undef READ

	for (int i = 0; i < n; i++) {
		snprintf(line, sizeof line, "instret-test %-28s %lu%s", r[i].what, (unsigned long)r[i].delta,
		         r[i].expect && r[i].delta != r[i].expect ? "  <-- unexpected" : "");
		port_text(line);
	}
#endif
}
