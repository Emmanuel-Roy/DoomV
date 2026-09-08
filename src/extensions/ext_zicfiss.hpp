#pragma once
// Zicfiss: the shadow stack, the backward-edge half of control-flow
// integrity.
//
// A function's return address is pushed to two places: the ordinary stack,
// where it can be overwritten by a buffer overrun, and a *shadow* stack the
// program cannot write with an ordinary store. On return, the two are
// compared. An attacker who has rewritten the stack copy therefore cannot
// make the return go anywhere, because the shadow copy still holds the
// truth and the mismatch raises a software-check exception.
//
// The mechanism is three pieces:
//
//   ssp        a CSR holding the shadow stack pointer.
//   four ops   sspush/sspopchk/ssrdp, which hide inside Zimop encodings,
//              and ssamoswap for the runtime to switch stacks.
//   a page     marked by the PTE encoding W=1 R=0, which is reserved --
//              and therefore a page fault -- on any hart without this
//              extension. That is what makes shadow stack pages
//              unwritable by ordinary stores while remaining writable by
//              the shadow stack instructions.
//
// Hiding the instructions in Zimop is what lets one binary run everywhere:
// on a hart without Zicfiss they are may-be-operations that write zero to
// rd and do nothing, so a program built with shadow stacks still runs, just
// without the protection.
#include "../registers.hpp"
#include "../extensions.hpp"
#include <cstdint>

namespace cfiss {

constexpr uint16_t CSR_SSP     = 0x011;
constexpr uint16_t CSR_MENVCFG = 0x30A;
constexpr uint16_t CSR_SENVCFG = 0x10A;
constexpr uint16_t CSR_HENVCFG = 0x60A;

constexpr uint64_t ENVCFG_SSE = 1ull << 3;

// Whether shadow stack instructions are enabled for the mode currently
// executing. The chain narrows as privilege drops, exactly as the
// cache-block and counter chains do: each level's envcfg governs the level
// below, and virtualisation inserts the hypervisor between them. M-mode is
// always enabled -- there is no envcfg above it to say otherwise.
inline bool enabled(Registers &regs)
{
	if (!Extensions.ZICFISS) return false;
	PrivMode priv = regs.get_priv();
	if (priv == PrivMode::M) return true;

	if (!(regs.read_csr(CSR_MENVCFG) & ENVCFG_SSE)) return false;
	const bool virt = Extensions.H && regs.get_virt();
	if (virt && !(regs.read_csr(CSR_HENVCFG) & ENVCFG_SSE)) return false;
	if (priv == PrivMode::U && !(regs.read_csr(CSR_SENVCFG) & ENVCFG_SSE))
		return false;
	return true;
}

// Whether a refusal is the hypervisor's rather than the machine's. Same
// distinction the other envcfg chains make: past menvcfg, a guest's
// refusal is one its hypervisor is positioned to emulate.
inline bool denial_is_virtual(Registers &regs)
{
	if (!Extensions.ZICFISS || !Extensions.H || !regs.get_virt()) return false;
	if (regs.get_priv() == PrivMode::M) return false;
	return (regs.read_csr(CSR_MENVCFG) & ENVCFG_SSE) != 0;
}

// The Zimop encodings Zicfiss claims. Both live in SYSTEM funct3=100.
//
//   MOP.RR.7  (funct7 0x67) with rd=x0 and rs1=x0   is SSPUSH rs2
//   MOP.R.28  (funct7 0x66, rs2=28) splits by operand:
//               rd=x0, rs1!=x0  is SSPOPCHK rs1
//               rd!=x0, rs1=x0  is SSRDP rd
//
// Anything else in those encodings stays an ordinary may-be-operation, and
// so does all of it when the extension is off -- which is the property that
// lets a shadow-stack binary run on a hart without shadow stacks.
inline bool is_sspush(const DecodedInstruction &i)
{
	return i.funct7 == 0x67 && i.rd == 0 && i.rs1 == 0;
}

inline bool is_sspopchk(const DecodedInstruction &i)
{
	return i.funct7 == 0x66 && i.rs2 == 28 && i.rd == 0 && i.rs1 != 0;
}

inline bool is_ssrdp(const DecodedInstruction &i)
{
	return i.funct7 == 0x66 && i.rs2 == 28 && i.rd != 0 && i.rs1 == 0;
}

} // namespace cfiss
