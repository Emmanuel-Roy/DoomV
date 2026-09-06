#pragma once
#include <cstdint>

class Registers;
class Memory;

// AMO gets its own tag (rather than reusing Store) because the priv spec
// requires an AMO's target page be both readable AND writable -- Store
// alone only requires W.
enum class AccessType : uint8_t {
	Fetch,
	Load,
	Store,
	Amo,
};

// Sv39 address translation. Stateless on purpose (no TLB) -- every call
// re-walks the page table directly out of guest RAM via `mem`, which is
// simple to get right and cheap enough for now; only worth revisiting if
// it's an actual measured bottleneck once something heavier than Doom is
// running.
//
// Returns true and fills `paddr` on a successful translation (including
// the trivial case: M-mode, or satp.MODE == 0, always succeeds untranslated
// -- see the .cpp for why MPRV isn't handled here yet). Returns false and
// fills `cause`/`tval` (a page-fault cause matching `type`, and the
// faulting virtual address) on failure, for the caller to pass straight
// into RiscvCore::enter_trap.
bool mmu_translate(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type,
                    uint64_t &paddr, uint64_t &cause, uint64_t &tval);
