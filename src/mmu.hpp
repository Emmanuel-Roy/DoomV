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
	// A shadow stack access (Zicfiss). It is the only kind that may touch
	// a page marked W=1 R=0, and the only kind that may write one -- an
	// ordinary store to such a page faults, which is the entire point:
	// the return addresses on it survive a buffer overrun because nothing
	// the program can write with reaches them.
	ShadowStack,
};

// Drops every cached translation. Must be called from anything that can
// change what a virtual address means: the fences (SFENCE.VMA, SINVAL.VMA,
// HFENCE.*), a write to satp/vsatp/hgatp, and a write to an envcfg whose
// bits gate whether a PTE faults. Getting that list wrong is the entire
// risk of having a cache at all -- a stale entry is a guest reading another
// process's memory, silently and much later.
void mmu_tlb_flush();

// Sv39/48/57 address translation.
//
// This used to be stateless on purpose, and the comment here said a TLB was
// worth revisiting only if it became "an actual measured bottleneck once
// something heavier than Doom is running". Something heavier turned up:
// booting Linux measures 1.67 MIPS, and building an Ubuntu root filesystem
// on this machine takes hours, nearly all of it re-walking the same page
// tables -- every fetch and every load or store was three or four dependent
// reads out of guest RAM before the access itself.
//
// So there is a small direct-mapped TLB now, and it caches deliberately
// little: only single-stage, non-virtualised, non-M-mode translations, and
// only ones whose A and D bits were already set, so that a hit cannot skip
// an update the architecture requires. Everything else -- two-stage,
// hlv/hsv, anything under H -- re-walks exactly as before, which keeps the
// hypervisor suite measuring the code it was written for.
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
