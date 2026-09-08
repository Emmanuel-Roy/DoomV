#pragma once
// Zicfilp: landing pads, the forward-edge half of control-flow integrity.
//
// The idea is small. Every indirect jump arms a one-bit expectation on the
// hart -- ELP, "expected landing pad" -- and the very next instruction to
// execute must be an `lpad`. Anything else raises a software-check
// exception. An attacker who has gained control of a jump target can
// therefore only land somewhere the compiler deliberately marked as a
// legitimate entry point, which rules out jumping into the middle of a
// function or into a gadget.
//
// `lpad` is not a new encoding. It is AUIPC with rd=x0 -- an instruction
// that computes a value and discards it, so on a hart without Zicfilp it
// is already a well-defined no-op. That is what lets the same binary run
// on both.
//
// Three things are worth being careful about:
//
//   * Not every indirect jump arms the expectation. A *return* does not:
//     returns are the backward edge, and Zicfiss guards those instead. Nor
//     does a software-guarded branch, which is how a compiler says "I know
//     where this goes".
//   * The label check is what makes landing pads more than a coarse
//     marker. A nonzero label must match the caller's, so a function
//     entry point cannot be reached by a call meant for a different
//     signature.
//   * ELP has to survive traps. It is live hart state at the moment an
//     interrupt arrives, so it is saved into the trapping mode's xPELP and
//     restored on the matching xRET -- otherwise an interrupt landing
//     between an indirect jump and its lpad would silently disarm the
//     check, which is precisely when an attacker would want it disarmed.
#include "../registers.hpp"
#include "../extensions.hpp"
#include <cstdint>

namespace cfilp {

constexpr uint16_t CSR_MSTATUS  = 0x300;
constexpr uint16_t CSR_VSSTATUS = 0x200;
constexpr uint16_t CSR_MENVCFG  = 0x30A;
constexpr uint16_t CSR_SENVCFG  = 0x10A;
constexpr uint16_t CSR_HENVCFG  = 0x60A;
constexpr uint16_t CSR_MSECCFG  = 0x747;

// The LPE bit sits at the same position in all three envcfg registers.
constexpr uint64_t ENVCFG_LPE = 1ull << 2;
// M-mode's own enable lives in mseccfg, since there is no envcfg above it.
constexpr uint64_t MSECCFG_MLPE = 1ull << 10;

// Where a saved ELP goes. SPELP shares bit 23 in both mstatus and vsstatus;
// MPELP is bit 41 and exists only in mstatus.
constexpr uint64_t STATUS_SPELP = 1ull << 23;
constexpr uint64_t STATUS_MPELP = 1ull << 41;

// Whether landing pads are enforced for the mode currently executing. Each
// level is governed by the level above it, which is the same shape every
// other envcfg field follows -- and it means a hypervisor can enforce
// landing pads on a guest that has not asked for them, but a guest cannot
// exempt itself from what its hypervisor requires.
inline bool enabled(Registers &regs)
{
	if (!Extensions.ZICFILP) return false;
	const bool virt = Extensions.H && regs.get_virt();
	switch (regs.get_priv()) {
	case PrivMode::M:
		return (regs.read_csr(CSR_MSECCFG) & MSECCFG_MLPE) != 0;
	case PrivMode::S:
		return virt ? (regs.read_csr(CSR_HENVCFG) & ENVCFG_LPE) != 0
		            : (regs.read_csr(CSR_MENVCFG) & ENVCFG_LPE) != 0;
	default:
		return (regs.read_csr(CSR_SENVCFG) & ENVCFG_LPE) != 0;
	}
}

// Whether a JALR arms the expectation.
//
// A return does not: `jalr x0, rs1` where rs1 is a link register (x1 or
// x5) is the backward edge, and guarding it is Zicfiss's job. A
// software-guarded branch -- rs1 = x7 -- does not either: that register is
// reserved for the compiler to say it has already established where the
// jump goes, which is how a switch-table dispatch avoids needing a landing
// pad at every case.
inline bool arms_expectation(uint8_t rd, uint8_t rs1)
{
	if (rd == 0 && (rs1 == 1 || rs1 == 5)) return false;  // return
	if (rs1 == 7) return false;                            // software-guarded
	return true;
}

} // namespace cfilp
