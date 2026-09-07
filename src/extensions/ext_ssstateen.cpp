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

// Which bits are writable depends on which state actually exists, and it
// differs per register. These masks were corrected against a reference
// after a first guess got all four wrong in different ways:
//
//   mstateen0   63 SE0, 62 ENVCFG, 60 JVT (Zcmt), 0 custom state
//   mstateen1-3 63 SE0 only -- the remaining bits name nothing yet
//   sstateen0-3 nothing. sstateen has no SE0 at all: U-mode has no
//               state-enable register, so there is no lower level to
//               aggregate, and the state its other bits would gate does
//               not exist here.
//   hstateen0-3 nothing, for the same reason as sstateen on this hart.
//
// Reporting an honest zero is the point. Software reads these to discover
// what it must save across a context switch, so a bit that reads back set
// claims state the machine does not have -- and a hypervisor would then
// save and restore nothing, believing it had done its job.
constexpr uint64_t STATEEN0_M_WMASK = (1ull << 63) | (1ull << 62) | (1ull << 60) | 1ull;
constexpr uint64_t STATEEN_M_WMASK  = (1ull << 63);
constexpr uint64_t STATEEN_SH_WMASK = 0;

uint64_t wmask_for(uint16_t csr)
{
	if (csr == CSR_MSTATEEN0) return STATEEN0_M_WMASK;
	if (csr > CSR_MSTATEEN0 && csr <= CSR_MSTATEEN0 + 3) return STATEEN_M_WMASK;
	return STATEEN_SH_WMASK; // sstateen* and hstateen*
}

bool is_stateen_csr(uint16_t csr)
{
	return (csr >= CSR_MSTATEEN0 && csr <= CSR_MSTATEEN0 + 3)
	    || (csr >= CSR_SSTATEEN0 && csr <= CSR_SSTATEEN0 + 3)
	    || (csr >= CSR_HSTATEEN0 && csr <= CSR_HSTATEEN0 + 3);
}

uint64_t read_stateen(Registers &regs, uint16_t csr)
{
	return regs.read_csr(csr) & wmask_for(csr);
}

void write_stateen(Registers &regs, uint16_t csr, uint64_t value)
{
	regs.write_csr(csr, value & wmask_for(csr));
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

} // namespace stateen
