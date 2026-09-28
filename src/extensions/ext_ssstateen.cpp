// Ssstateen: the state-enable CSRs.
//
// A set of four registers per privilege level (mstateen0-3, sstateen0-3,
// hstateen0-3) whose bits gate access to state that newer extensions add.
// A bit clear means the state below is unavailable to lower privilege
// levels, and touching it raises an illegal instruction -- or, from a
// guest, a virtual instruction.
//
// The problem it solves is specific and worth stating, because the
// registers look like pointless bureaucracy otherwise. When a hypervisor
// switches between guests it must save and restore every piece of
// architectural state a guest might have touched. Each new extension adds
// more, and a hypervisor written before that extension existed does not
// know to save it -- so a guest using it would silently corrupt another
// guest's state across a context switch. The stateen bits let the
// hypervisor deny access to state it does not understand, turning a silent
// corruption into a clean trap.
//
// This hart implements the CSRs and their gating, but most bits are
// read-only zero because the state they would gate does not exist here.
// Exactly which bits survive differs per register, and the masks below say
// so -- see the comment there, and note that a first guess got all four
// wrong in different ways.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "extensions.hpp"
#include "ext_ssstateen.hpp"
#include <cstdint>

namespace stateen {

// Which bits are writable depends on which state actually exists on this
// hart -- these fields are WARL, and each bit names a specific extension's
// state. That makes the mask a statement about DoomV, not something to be
// copied from a reference.
//
// It was copied from a reference, initially, and that was the error: spike
// allows bits 60 (Zcmt's JVT) and 0 (custom state), which DoomV does not
// implement. The two references disagree with each other here precisely
// because they implement different things, and matching either one would
// have claimed state this machine does not have.
//
// That reasoning was then applied to one bit it did not fit. This comment
// used to dismiss bit 55 as "CSRIND, which DoomV does not implement
// either" -- but bit 55 is SRMCFG; CSRIND is bit 60. DoomV *does*
// implement srmcfg, and clearing its gate made the register permanently
// unreachable from HS-mode: `csr_access_permitted` tests mstateen0[55]
// before allowing srmcfg below M, and nothing could ever set it. A rule
// enforced against a value nothing could write, which is the same shape as
// the hstateen0 bug recorded further down.
//
// What DoomV actually has:
//
//   63 SE0     the aggregate gate for the next lower level. Required, and
//              what the whole hierarchy below depends on.
//   62 ENVCFG  menvcfg/senvcfg exist here -- Svpbmt, Sstc and pointer
//              masking all read them.
//   55 SRMCFG  Ssqosid's srmcfg exists here. Present in mstateen0 only:
//              hstateen0 has no SRMCFG bit at all, which is what makes
//              srmcfg unreachable from every virtual mode no matter what
//              the hypervisor writes -- resource-control identities are
//              assigned *to* guests, never by them.
//
// Everything else is read-only zero: no Zcmt jump table, no Zfinx, no
// Sdtrig context registers, no custom state. Reporting that honestly is
// the point of the register -- software reads it to discover what it must
// save across a context switch, and a bit that reads back set would have a
// hypervisor saving and restoring nothing.
//
// sstateen has no SE0 at all: U-mode has no state-enable register, so
// there is no lower level to aggregate. hstateen0's SE0 would gate a
// guest's access to sstateen, but with sstateen itself empty here there is
// nothing for it to gate.
//
// mstateen1-3 are read-only zero for the same reason, SE0 included. Their
// SE0 gates sstateen1-3, which are empty on this hart, so a writable gate
// would advertise control over nothing. The references split on this --
// spike keeps SE0 writable, Sail hardwires it -- which is itself the
// evidence that it is implementation-defined rather than specified, and
// the honest answer for DoomV is the one that matches what it has.
constexpr uint64_t STATEEN0_M_WMASK = (1ull << 63) | (1ull << 62) | (1ull << 55);
constexpr uint64_t STATEEN_M_WMASK  = 0;
// sstateen* holds nothing on this hart: U-mode has no state-enable
// register below it to aggregate, and none of the state sstateen's other
// bits would gate (Zcmt's jump table, Zfinx, Sdtrig context) exists here.
constexpr uint64_t STATEEN_S_WMASK  = 0;
// hstateen0 implements what mstateen0 implements. It has to: SE0 is the
// hypervisor's gate over its guest's sstateen, and the code below reads
// that bit to decide access. With hstateen hardwired to zero, the gate
// could never be opened, so a guest was refused sstateen unconditionally
// and the bit was checked but unreachable -- a rule enforced against a
// value nothing could set.
constexpr uint64_t STATEEN0_H_WMASK = (1ull << 63) | (1ull << 62);
constexpr uint64_t STATEEN_H_WMASK  = 0;

uint64_t wmask_for(uint16_t csr)
{
	if (csr == CSR_MSTATEEN0) return STATEEN0_M_WMASK;
	if (csr > CSR_MSTATEEN0 && csr <= CSR_MSTATEEN0 + 3) return STATEEN_M_WMASK;
	if (csr == CSR_HSTATEEN0) return STATEEN0_H_WMASK;
	if (csr > CSR_HSTATEEN0 && csr <= CSR_HSTATEEN0 + 3) return STATEEN_H_WMASK;
	return STATEEN_S_WMASK;
}

// A bit is read-only zero at one level when the same bit is zero at the
// level above. That is what makes the hierarchy a withholding mechanism
// rather than three independent registers: M clearing a bit takes it away
// from HS *and* from the guest, with no way for either to put it back.
uint64_t enclosing_mask(Registers &regs, uint16_t csr)
{
	if (csr >= CSR_HSTATEEN0 && csr <= CSR_HSTATEEN0 + 3)
		return regs.read_csr((uint16_t)(CSR_MSTATEEN0 + (csr - CSR_HSTATEEN0)));
	if (csr >= CSR_SSTATEEN0 && csr <= CSR_SSTATEEN0 + 3) {
		int i = csr - CSR_SSTATEEN0;
		uint64_t m = regs.read_csr((uint16_t)(CSR_MSTATEEN0 + i));
		// A guest's view is narrowed again by the hypervisor's register.
		if (Extensions.H && regs.get_virt())
			m &= regs.read_csr((uint16_t)(CSR_HSTATEEN0 + i));
		return m;
	}
	return ~0ull;
}

bool is_stateen_csr(uint16_t csr)
{
	return (csr >= CSR_MSTATEEN0 && csr <= CSR_MSTATEEN0 + 3)
	    || (csr >= CSR_SSTATEEN0 && csr <= CSR_SSTATEEN0 + 3)
	    || (csr >= CSR_HSTATEEN0 && csr <= CSR_HSTATEEN0 + 3);
}

uint64_t read_stateen(Registers &regs, uint16_t csr)
{
	return regs.read_csr(csr) & wmask_for(csr) & enclosing_mask(regs, csr);
}

void write_stateen(Registers &regs, uint16_t csr, uint64_t value)
{
	// Bits the level above has cleared are read-only zero, so a write
	// cannot set them -- but it must not clear the ones already set
	// either, since those are simply not this register's to touch.
	uint64_t writable = wmask_for(csr) & enclosing_mask(regs, csr);
	regs.write_csr(csr, (regs.read_csr(csr) & ~writable) | (value & writable));
}

// Whether a lower privilege level may reach a given stateen register.
//
// The hierarchy is the point: a bit in mstateen gates the corresponding
// sstateen and hstateen register, so M-mode can withhold from S-mode
// something S-mode would otherwise be free to hand to a guest. Denying at
// the top denies all the way down, which is what makes the mechanism
// useful for a hypervisor that does not trust what it does not know.
bool stateen_access_permitted(Registers &regs, uint16_t csr)
{
	PrivMode priv = regs.get_priv();
	if (priv == PrivMode::M) return true; // M-mode is never gated

	// Reaching sstateen or hstateen from below M requires mstateen's SE0
	// bit for the same index.
	int index = 0;
	if (csr >= CSR_SSTATEEN0 && csr <= CSR_SSTATEEN0 + 3) index = csr - CSR_SSTATEEN0;
	else if (csr >= CSR_HSTATEEN0 && csr <= CSR_HSTATEEN0 + 3) index = csr - CSR_HSTATEEN0;
	else return false; // an mstateen register, and we are not in M-mode

	uint64_t mstateen = regs.read_csr((uint16_t)(CSR_MSTATEEN0 + index));
	if (!(mstateen & (1ull << 63))) return false;

	// A guest reaching sstateen additionally needs hstateen's SE0 bit --
	// the hypervisor's own say over what its guest may see.
	if (Extensions.H && regs.get_virt()) {
		uint64_t hstateen = regs.read_csr((uint16_t)(CSR_HSTATEEN0 + index));
		if (!(hstateen & (1ull << 63))) return false;
	}
	return true;
}

// Which exception a denied access raises. The two gates answer
// differently, and the difference is the whole point of having both: a
// guest refused by *mstateen* has been refused by the machine, and gets an
// illegal instruction; a guest refused by *hstateen* has been refused by
// its hypervisor, and gets a virtual instruction, which is the exception
// the hypervisor can catch and emulate on the guest's behalf. Reporting
// cause 2 for the second kind tells the guest it did something impossible
// when in fact its supervisor was simply not willing.
bool stateen_denial_is_virtual(Registers &regs, uint16_t csr)
{
	if (!Extensions.H || !regs.get_virt()) return false;

	int index;
	if (csr >= CSR_SSTATEEN0 && csr <= CSR_SSTATEEN0 + 3) index = csr - CSR_SSTATEEN0;
	else if (csr >= CSR_HSTATEEN0 && csr <= CSR_HSTATEEN0 + 3) index = csr - CSR_HSTATEEN0;
	else return false;

	// M-mode's refusal takes precedence: if mstateen closed it, hstateen
	// never got a say, and the guest is not being denied by its host.
	if (!(regs.read_csr((uint16_t)(CSR_MSTATEEN0 + index)) & (1ull << 63))) return false;
	return true;
}

} // namespace stateen
