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
	// A cache-block operation (Zicbom's cbo.clean/flush/inval) is none of
	// the above, and the differences are not cosmetic:
	//
	//   * read *or* write permission suffices, for all three -- a
	//     read-only mapping may be cleaned, flushed and invalidated.
	//   * the D bit is neither required nor set, because nothing is
	//     written.
	//   * a failure is reported as a *store* page or access fault anyway,
	//     whichever permission was the one missing.
	//
	// Modelling it as a Store demands write permission and the D bit;
	// modelling it as a Load reports the wrong cause. Hence its own tag.
	CacheBlock,
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
// as_guest makes the walk use the *guest's* translation rather than the
// current mode's: vsatp instead of satp, and the guest's privilege from
// hstatus.SPVP rather than the hart's own. It is what hlv/hsv need -- they
// execute in HS-mode but must reach memory exactly as the guest would,
// which is the whole reason those instructions exist rather than the
// hypervisor simply dereferencing a pointer.
bool mmu_translate(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type,
                    uint64_t &paddr, uint64_t &cause, uint64_t &tval, bool as_guest = false);
