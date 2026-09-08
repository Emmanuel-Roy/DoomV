#pragma once
// The envcfg gate that governs the cache-block instructions.
//
// Zicbom's cbo.inval/clean/flush and Zicboz's cbo.zero are each controlled
// by a field in the envcfg register chain, and the chain narrows as
// privilege drops in exactly the way the counter-enable chain does: M-mode
// decides what S-mode may do, S-mode decides for U-mode, and under
// virtualisation the hypervisor's henvcfg sits between them.
//
// This exists because the cache-block instructions had no gate at all.
// Having no cache makes their *effect* trivial, which is what the original
// implementation modelled -- but whether software is permitted to issue
// them is observable regardless of what happens behind them, and it is the
// whole reason the fields exist. A hypervisor clears CBIE precisely so
// that a guest's cbo.inval traps to it.
#include "../registers.hpp"
#include "../extensions.hpp"
#include <cstdint>

namespace cbo {

constexpr uint16_t CSR_MENVCFG = 0x30A;
constexpr uint16_t CSR_SENVCFG = 0x10A;
constexpr uint16_t CSR_HENVCFG = 0x60A;

// The three fields, by the instruction they gate.
//   CBIE  (bits 5:4) cbo.inval -- a *field*, not a flag: 0 traps, 1 means
//                    "execute as a flush", 3 means invalidate. There is no
//                    separate enable, so "permitted" is "nonzero".
//   CBCFE (bit 6)    cbo.clean and cbo.flush
//   CBZE  (bit 7)    cbo.zero
enum Field { CBIE, CBCFE, CBZE };

inline bool field_permits(uint64_t envcfg, Field f)
{
	switch (f) {
	case CBIE:  return ((envcfg >> 4) & 0x3) != 0;
	case CBCFE: return (envcfg & (1ull << 6)) != 0;
	default:    return (envcfg & (1ull << 7)) != 0;
	}
}

enum Result {
	ALLOW,
	DENY_ILLEGAL,   // the machine's refusal, or a guest kernel's own
	DENY_VIRTUAL,   // the hypervisor's refusal -- it can emulate this
};

// senvcfg is read for U-mode and VU-mode alike: the H extension defines no
// vsenvcfg, so a guest's senvcfg *is* senvcfg, context-switched by the
// hypervisor like the rest of the guest's supervisor state.
inline Result check(Registers &regs, Field f)
{
	PrivMode priv = regs.get_priv();
	if (priv == PrivMode::M) return ALLOW;

	if (!field_permits(regs.read_csr(CSR_MENVCFG), f)) return DENY_ILLEGAL;

	const bool virt = Extensions.H && regs.get_virt();
	// Past this point the machine has allowed the operation, so any
	// remaining refusal inside a guest is one the hypervisor is positioned
	// to emulate -- which is what makes it a virtual instruction rather
	// than an illegal one.
	if (virt && !field_permits(regs.read_csr(CSR_HENVCFG), f))
		return DENY_VIRTUAL;

	if (priv == PrivMode::U && !field_permits(regs.read_csr(CSR_SENVCFG), f))
		return virt ? DENY_VIRTUAL : DENY_ILLEGAL;

	return ALLOW;
}

} // namespace cbo
