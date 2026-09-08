// H: the hypervisor extension -- CSR file and privilege plumbing.
//
// This is the first of several increments. It implements the state a
// hypervisor needs to exist at all, and deliberately stops short of
// two-stage address translation, hlv/hsv, and htinst, each of which gets
// its own step so each can be checked against spike on its own.
//
// The central idea is that virtualisation is *orthogonal* to privilege
// rather than another level of it. The hart is in one of M, HS, VS, HU or
// VU, which is a (privilege, virtual) pair -- not a five-valued privilege
// enum. Registers keeps them separate for that reason: VS-mode code is
// still PrivMode::S, so every existing comparison against S keeps working
// untouched, and only code that genuinely cares about virtualisation asks
// get_virt().
//
// Two consequences of that shape drive most of this file:
//
//   * In VS-mode, the S-mode CSR *names* do not refer to the S-mode CSRs.
//     A guest reading "sstatus" gets vsstatus, "satp" gets vsatp, and so
//     on. The guest is not aware it is being redirected -- that is the
//     whole point -- so the redirection has to happen at the CSR number,
//     before anything else looks at it.
//
//   * The hypervisor's own registers must be invisible to the guest.
//     Accessing hstatus from VS-mode is not an illegal instruction but a
//     *virtual instruction* exception (cause 22), which is a different
//     cause precisely so a hypervisor can tell "the guest tried to do
//     something only I may do" apart from "the guest executed garbage".
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

namespace hyp {

// Hypervisor CSRs, checked against spike's encoding.h rather than recalled.
constexpr uint16_t CSR_HSTATUS    = 0x600;
constexpr uint16_t CSR_HEDELEG    = 0x602;
constexpr uint16_t CSR_HIDELEG    = 0x603;
constexpr uint16_t CSR_HIE        = 0x604;
constexpr uint16_t CSR_HTIMEDELTA = 0x605;
constexpr uint16_t CSR_HCOUNTEREN = 0x606;
constexpr uint16_t CSR_HGEIE      = 0x607;
constexpr uint16_t CSR_HENVCFG    = 0x60A;
constexpr uint16_t CSR_HTVAL      = 0x643;
constexpr uint16_t CSR_HIP        = 0x644;
constexpr uint16_t CSR_HVIP       = 0x645;
constexpr uint16_t CSR_HTINST     = 0x64A;
constexpr uint16_t CSR_HGATP      = 0x680;
constexpr uint16_t CSR_HGEIP      = 0xE12;

// The VS-mode shadows of the S-mode registers.
constexpr uint16_t CSR_VSSTATUS  = 0x200;
constexpr uint16_t CSR_VSIE      = 0x204;
constexpr uint16_t CSR_VSTVEC    = 0x205;
constexpr uint16_t CSR_VSSCRATCH = 0x240;
constexpr uint16_t CSR_VSEPC     = 0x241;
constexpr uint16_t CSR_VSCAUSE   = 0x242;
constexpr uint16_t CSR_VSTVAL    = 0x243;
constexpr uint16_t CSR_VSIP      = 0x244;
constexpr uint16_t CSR_VSATP     = 0x280;

// hstatus fields.
constexpr uint64_t HSTATUS_VSBE  = 1ull << 5;
constexpr uint64_t HSTATUS_GVA   = 1ull << 6;
constexpr uint64_t HSTATUS_SPV   = 1ull << 7;
constexpr uint64_t HSTATUS_SPVP  = 1ull << 8;
constexpr uint64_t HSTATUS_HU    = 1ull << 9;
constexpr uint64_t HSTATUS_VGEIN = 0x3Full << 12;
constexpr uint64_t HSTATUS_VTVM  = 1ull << 20;
constexpr uint64_t HSTATUS_VTW   = 1ull << 21;
constexpr uint64_t HSTATUS_VTSR  = 1ull << 22;
// VSXL is read-only 2 (64-bit) here: this hart has no 32-bit VS mode.
constexpr uint64_t HSTATUS_VSXL  = 3ull << 32;
constexpr uint64_t HSTATUS_VSXL_64 = 2ull << 32;

// Writable bits. Everything else reads as zero, which is what makes a
// hypervisor's feature probing give an honest answer instead of reading
// back whatever it happened to write.
//
// Two fields are deliberately absent, both read-only zero:
//
//   VSBE  -- selects big-endian VS-mode. This hart is little-endian only,
//            and a writable VSBE would promise an endianness it cannot
//            actually switch to.
//   VGEIN -- selects which guest external interrupt is visible. GEILEN is
//            zero here: there is no guest external interrupt controller at
//            all, so any nonzero VGEIN would name something absent.
//
// HUPMM is present, but conditionally: it is the hypervisor's half of
// pointer masking (Ssnpm), selecting the PMLEN applied to the addresses
// hlv/hlvx/hsv compute. It is writable only when this hart implements
// pointer masking at all -- advertising a masking control on a machine
// that cannot mask would be the same lie VSBE would be.
constexpr uint64_t HSTATUS_HUPMM = 3ull << 48;
constexpr uint64_t HSTATUS_WMASK_BASE = HSTATUS_GVA | HSTATUS_SPV | HSTATUS_SPVP
                                      | HSTATUS_HU | HSTATUS_VTVM | HSTATUS_VTW
                                      | HSTATUS_VTSR;
inline uint64_t hstatus_wmask()
{
	return HSTATUS_WMASK_BASE | (Extensions.SSNPM ? HSTATUS_HUPMM : 0);
}

// mstatus's virtualisation fields, both above bit 32 and so RV64-only.
constexpr uint64_t MSTATUS_GVA = 1ull << 38;
constexpr uint64_t MSTATUS_MPV = 1ull << 39;

// Guest-page-fault and virtual-instruction causes. These exist as distinct
// numbers so a hypervisor can tell a second-stage translation failure apart
// from a first-stage one, and a privileged-operation attempt apart from an
// illegal encoding.
constexpr uint64_t CAUSE_INST_GUEST_PAGE_FAULT  = 20;
constexpr uint64_t CAUSE_VIRTUAL_INSTRUCTION    = 22;
constexpr uint64_t CAUSE_LOAD_GUEST_PAGE_FAULT  = 21;
constexpr uint64_t CAUSE_STORE_GUEST_PAGE_FAULT = 23;

bool is_hypervisor_csr(uint16_t csr)
{
	if (csr >= CSR_HSTATUS && csr <= CSR_HENVCFG) return true;
	if (csr >= CSR_HTVAL && csr <= CSR_HTINST) return true;
	return csr == CSR_HGATP || csr == CSR_HGEIP;
}

bool is_vs_csr(uint16_t csr)
{
	return (csr >= CSR_VSSTATUS && csr <= CSR_VSIP) || csr == CSR_VSATP;
}

// In VS-mode the S-mode CSR numbers refer to the VS shadows instead. The
// guest issues `csrr t0, satp` and must transparently get vsatp; it has no
// way to reach the real satp, which belongs to the hypervisor.
//
// Doing this by rewriting the CSR number -- rather than special-casing each
// register at its read and write sites -- is what keeps it from being
// forgotten somewhere. Every path that interprets a CSR number goes through
// here first.
uint16_t redirect_for_virt(Registers &regs, uint16_t csr)
{
	if (!regs.get_virt()) return csr;
	switch (csr) {
	case 0x100: return CSR_VSSTATUS;  // sstatus
	case 0x104: return CSR_VSIE;      // sie
	case 0x105: return CSR_VSTVEC;    // stvec
	case 0x140: return CSR_VSSCRATCH; // sscratch
	case 0x141: return CSR_VSEPC;     // sepc
	case 0x142: return CSR_VSCAUSE;   // scause
	case 0x143: return CSR_VSTVAL;    // stval
	case 0x106: return 0x206;         // scounteren -> vscounteren
	case 0x144: return CSR_VSIP;      // sip
	case 0x180: return CSR_VSATP;     // satp
	default: return csr;
	}
}

// Whether an access is a *virtual* instruction rather than an illegal one.
//
// The distinction matters to a hypervisor: cause 2 means the guest ran
// something no one could run, while cause 22 means it ran something only
// the hypervisor may run, which the hypervisor can then emulate on the
// guest's behalf. Returning the wrong one turns an emulable trap into a
// crash.
bool is_virtual_instruction_csr(Registers &regs, uint16_t csr)
{
	if (!regs.get_virt()) return false;
	// A guest touching the hypervisor's own registers, or the VS shadows
	// directly (it must use the S-mode names, which get redirected), is
	// attempting something only HS-mode may do.
	return is_hypervisor_csr(csr) || is_vs_csr(csr);
}

uint64_t read_hstatus(Registers &regs)
{
	// VSXL is read-only: this hart's VS mode is always 64-bit.
	return (regs.read_csr(CSR_HSTATUS) & hstatus_wmask()) | HSTATUS_VSXL_64;
}

void write_hstatus(Registers &regs, uint64_t value)
{
	uint64_t updated = value & hstatus_wmask();
	// HUPMM is WARL on the same terms as the envcfg PMM fields: 0 off, 2
	// PMLEN=7, 3 PMLEN=16, and 1 reserved. A reserved value must not read
	// back, so an attempt to write it keeps the field it had.
	if ((((updated >> 48) & 0x3) == 1))
		updated = (updated & ~HSTATUS_HUPMM)
		        | (regs.read_csr(CSR_HSTATUS) & HSTATUS_HUPMM);
	regs.write_csr(CSR_HSTATUS, updated);
}

} // namespace hyp
