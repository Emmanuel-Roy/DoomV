#include "ext_zicntr.hpp"
#include "extensions.hpp"

namespace counters {

// This machine has exactly one clock. mtime is incremented once per retired
// instruction (Memory::step_instructions -> Timer::tick), which is what makes
// the timer deterministic and a hand-written test able to compute an exact
// fire time -- see timer.hpp. So the retired-instruction count, the cycle
// count and the wall clock are all literally the same number here, and
// cycle/time/instret return it.
//
// That is not a shortcut around three separate counters; it is what this
// implementation's timing model actually is. An interpreter that retires
// exactly one instruction per step has cycle == instret by construction,
// and mtime is defined off the same step. The one property software relies
// on -- that each is monotonically non-decreasing and advances as the
// program runs -- holds.
//
// Zihpm's hpmcounter3..31 are hardwired to zero. The spec permits any of
// them to be read-only zero, which is the honest answer for a machine that
// counts no events: reporting a made-up number would be worse than
// reporting none. They still have to be *readable* rather than illegal,
// which is the part that matters for conformance.
uint64_t read_counter(Registers &regs, Memory &mem, uint16_t csr)
{
	(void)regs;
	switch (csr) {
	case CSR_CYCLE:
	case CSR_TIME:
	case CSR_INSTRET:
		return mem.get_timer().get_mtime();
	default:
		return 0; // hpmcounter3..31
	}
}

// The counter-enable chain, per the privileged spec: a counter is readable
// in S-mode only if its bit is set in mcounteren, and in U-mode only if the
// bit is set in *both* mcounteren and scounteren. M-mode always reads them.
//
// Getting the U-mode case wrong in the permissive direction is the easy
// mistake -- checking scounteren alone would let U read a counter M-mode
// had deliberately withheld, since scounteren is written by the kernel and
// mcounteren by firmware. The chain only ever narrows as privilege drops.
bool counter_permitted(Registers &regs, uint16_t csr)
{
	PrivMode priv = regs.get_priv();
	if (priv == PrivMode::M) return true;

	uint64_t bit = 1ull << counter_index(csr);
	if (!(regs.read_csr(CSR_MCOUNTEREN) & bit)) return false;

	// Under virtualisation the chain gains a link. hcounteren is the
	// hypervisor's say over what its guest may read, and it sits between
	// mcounteren and the guest's own scounteren: a counter reaches VU-mode
	// only if the machine, the hypervisor *and* the guest kernel all allow
	// it. Leaving hcounteren out of the chain let a guest read counters
	// the hypervisor had withheld -- and hcounteren is precisely the
	// register a hypervisor uses to stop a guest timing the host.
	//
	// The guest's supervisor gate is scounteren itself. The H extension
	// defines no vscounteren -- scounteren is a single register holding
	// whichever supervisor's values are current, context-switched by the
	// hypervisor along with the rest of the guest's supervisor state, and
	// substituting a VS-numbered register here denies VU-mode a counter
	// its own kernel had enabled.
	if (Extensions.H && regs.get_virt()) {
		if (!(regs.read_csr(CSR_HCOUNTEREN) & bit)) return false;
		if (priv == PrivMode::U && !(regs.read_csr(CSR_SCOUNTEREN) & bit))
			return false;
		return true;
	}

	if (priv == PrivMode::U && !(regs.read_csr(CSR_SCOUNTEREN) & bit)) return false;
	return true;
}

// Which exception a refused counter raises. The three gates do not answer
// alike, and the difference is the same one the state-enable registers
// make: a refusal by *hcounteren* is the hypervisor's, and the guest gets a
// virtual instruction so the hypervisor can emulate the read on its behalf.
// A refusal by mcounteren is the machine's, and a refusal by the guest's
// own vscounteren is the guest kernel's -- both are cause 2, because
// neither has a hypervisor underneath waiting to step in.
bool counter_denial_is_virtual(Registers &regs, uint16_t csr)
{
	if (!Extensions.H || !regs.get_virt()) return false;
	if (regs.get_priv() == PrivMode::M) return false;

	uint64_t bit = 1ull << counter_index(csr);
	// M-mode's refusal takes precedence: if mcounteren closed it, the
	// hypervisor never got a say and the guest is not being denied by it.
	if (!(regs.read_csr(CSR_MCOUNTEREN) & bit)) return false;
	return (regs.read_csr(CSR_HCOUNTEREN) & bit) == 0;
}

} // namespace counters
