// Sscofpmf: count-overflow and mode-based filtering for the hardware
// performance counters.
//
// Two mechanisms, both about making performance counters usable from
// supervisor mode without M-mode having to mediate every sample:
//
//   * Each mhpmeventN gains an OF (overflow) bit and a set of inhibit bits
//     that filter which privilege modes the counter counts in. When a
//     counter overflows, OF is set and a local counter-overflow interrupt
//     (LCOFI, interrupt 13) is raised.
//
//   * scountovf exposes, read-only to S-mode, which counters have
//     overflowed -- so a profiler can find the one that fired without
//     needing access to the mhpmevent registers themselves.
//
// The point is sampled profiling: set a counter to overflow after N events,
// take the interrupt, record where the program counter was. Without it a
// profiler has to poll, which perturbs what it is measuring.
//
// This hart counts no events. hpmcounter3..31 are hardwired to zero (see
// ext_zicntr.cpp), so nothing ever increments and no overflow can occur
// spontaneously. That does not make the extension vacuous here: the
// registers, their WARL behaviour, the interrupt plumbing and scountovf's
// read-only view are all real and observable, and software can still drive
// an overflow by writing a counter directly. What is absent is a source of
// events to count, which is a property of the machine rather than of the
// extension.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "extensions.hpp"
#include "ext_sscofpmf.hpp"
#include <cstdint>

namespace sscofpmf {

// mhpmeventN's high bits. OF is bit 63; the four inhibit bits below it say
// which privilege modes the counter does *not* count in.
constexpr uint64_t MHPMEVENT_OF    = 1ull << 63;
constexpr uint64_t MHPMEVENT_MINH  = 1ull << 62;
constexpr uint64_t MHPMEVENT_SINH  = 1ull << 61;
constexpr uint64_t MHPMEVENT_UINH  = 1ull << 60;
constexpr uint64_t MHPMEVENT_VSINH = 1ull << 59;
constexpr uint64_t MHPMEVENT_VUINH = 1ull << 58;

bool is_mhpmevent(uint16_t csr)
{
	return csr >= CSR_MHPMEVENT3 && csr <= CSR_MHPMEVENT31;
}

// scountovf is a read-only window onto the OF bits of every mhpmevent,
// packed by counter index: bit N reflects mhpmeventN.OF.
//
// It is assembled on read rather than stored, because it has no state of
// its own -- storing a copy would let the two disagree, and the disagreement
// would only show up when a profiler read the wrong one.
uint64_t read_scountovf(Registers &regs)
{
	uint64_t v = 0;
	for (int n = 3; n <= 31; n++) {
		uint64_t ev = regs.read_csr((uint16_t)(CSR_MHPMEVENT3 + (n - 3)));
		if (ev & MHPMEVENT_OF) v |= 1ull << n;
	}

	// S-mode only sees counters mcounteren permits it, and a guest only
	// those hcounteren permits as well. Showing an overflow for a counter
	// the reader cannot access would leak which counters M-mode is using.
	if (regs.get_priv() != PrivMode::M) {
		v &= regs.read_csr(CSR_MCOUNTEREN);
		if (Extensions.H && regs.get_virt()) v &= regs.read_csr(CSR_HCOUNTEREN);
	}
	return v;
}

// The OF bit is writable (software clears it after handling an overflow),
// as are the inhibit bits. The event selector itself is the low 58 bits,
// which this hart keeps but never interprets -- there are no events to
// select from.
uint64_t mhpmevent_wmask()
{
	return MHPMEVENT_OF | MHPMEVENT_MINH | MHPMEVENT_SINH | MHPMEVENT_UINH
	     | MHPMEVENT_VSINH | MHPMEVENT_VUINH | 0x03FFFFFFFFFFFFFFull;
}

// Whether any counter is currently signalling an overflow. Drives the LCOFI
// interrupt bit in mip, the same way the timer drives MTIP -- the interrupt
// is a function of state rather than an edge that has to be latched, which
// keeps it consistent if software clears OF while the handler is running.
bool lcofi_pending(Registers &regs)
{
	for (int n = 3; n <= 31; n++) {
		if (regs.read_csr((uint16_t)(CSR_MHPMEVENT3 + (n - 3))) & MHPMEVENT_OF)
			return true;
	}
	return false;
}

} // namespace sscofpmf
