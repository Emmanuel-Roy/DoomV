// Zicsr extension: ECALL/EBREAK/MRET control-transfer plus CSR read/modify/
// write. Also owns the M-mode trap-entry sequence (enter_trap), since ECALL/
// EBREAK are the only things in this project that ever trigger one.
#include "ext_h.hpp"
#include "ext_sscofpmf.hpp"
#include <cstdio>
#include <cstdlib>
#include "ext_ssstateen.hpp"
#include "ext_zicntr.hpp"
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include "timer.hpp"
#include "pmp.hpp"
#include "ext_xstate.hpp"
#include "imsic.hpp"

DecodedInstruction Decoder::decode_zicsr(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZICSR;
	instr.length = 4;
	instr.mnemonic = "???";

	uint8_t opcode = raw_instr & 0x7F;
	uint8_t rd     = (raw_instr >> 7) & 0x1F;
	uint8_t funct3 = (raw_instr >> 12) & 0x07;
	uint8_t rs1    = (raw_instr >> 15) & 0x1F;
	uint8_t rs2    = (raw_instr >> 20) & 0x1F;
	uint8_t funct7 = (raw_instr >> 25) & 0x7F;

	instr.opcode = opcode;
	instr.rd = rd;
	instr.rs1 = rs1;
	instr.rs2 = rs2;
	instr.funct3 = funct3;
	instr.funct7 = funct7;

	// imm[11:0] here is a CSR address (0-4095), not a signed immediate --
	// reusing a sign-extended imm_i would corrupt any address with bit 11
	// set (e.g. mhartid = 0xF14).
	uint32_t csr_or_funct12 = (raw_instr >> 20) & 0xFFF;
	instr.imm = (int64_t)csr_or_funct12;
	if (funct3 == 0b000) {
		if (raw_instr == 0x00000073) instr.mnemonic = "ECALL";
		else if (raw_instr == 0x00100073) instr.mnemonic = "EBREAK";
		else if (raw_instr == 0x30200073) instr.mnemonic = "MRET";
		else if (raw_instr == 0x10200073) instr.mnemonic = "SRET";
		else if (raw_instr == 0x10500073) instr.mnemonic = "WFI";
		else if (raw_instr == 0x00D00073) { instr.mnemonic = "WRS.NTO"; instr.ext = Extension::ZAWRS; }
		else if (raw_instr == 0x01D00073) { instr.mnemonic = "WRS.STO"; instr.ext = Extension::ZAWRS; }
		else if (funct7 == 0b0001001 && rd == 0) instr.mnemonic = "SFENCE.VMA";
		else if (funct7 == 0b0001011 && rd == 0) { instr.mnemonic = "SINVAL.VMA"; instr.ext = Extension::SVINVAL; }
		else if (funct7 == 0b0001100 && rd == 0 && rs1 == 0 && rs2 == 0) { instr.mnemonic = "SFENCE.W.INVAL"; instr.ext = Extension::SVINVAL; }
		else if (funct7 == 0b0001100 && rd == 0 && rs1 == 0 && rs2 == 1) { instr.mnemonic = "SFENCE.INVAL.IR"; instr.ext = Extension::SVINVAL; }
		// else: genuinely unrecognized SYSTEM encoding -- mnemonic stays
		// "???", exec_32ZICSR's default case no-ops it the same as before.
	} else {
		switch (funct3) {
		case 0b001: instr.mnemonic = "CSRRW";  break;
		case 0b010: instr.mnemonic = "CSRRS";  break;
		case 0b011: instr.mnemonic = "CSRRC";  break;
		case 0b101: instr.mnemonic = "CSRRWI"; break;
		case 0b110: instr.mnemonic = "CSRRSI"; break;
		case 0b111: instr.mnemonic = "CSRRCI"; break;
		}
	}

	return instr;
}

namespace {
// M-mode CSR addresses actually given meaning by exec_32ZICSR/enter_trap.
// Anything else (mscratch, mhartid, ...) is still fully readable/writable
// -- Registers::csr[] backs all 4096 addresses generically -- it just has
// no side effects, which is correct for those. misa (below) and satp
// (see write_satp) are the two exceptions in this block: both need real
// side effects, not just a name.
constexpr uint16_t CSR_MISA    = 0x301;
constexpr uint16_t CSR_MSTATUS = 0x300;
constexpr uint16_t CSR_MEDELEG = 0x302;
constexpr uint16_t CSR_MIDELEG = 0x303;
constexpr uint16_t CSR_MTVEC   = 0x305;
constexpr uint16_t CSR_MEPC    = 0x341;
constexpr uint16_t CSR_MCAUSE  = 0x342;
constexpr uint16_t CSR_MTVAL   = 0x343;

// S-mode CSRs. sstatus (0x100) isn't listed here -- it's a masked view of
// mstatus, not separate storage (see read_sstatus/write_sstatus below).
// sedeleg/sideleg (delegating below S, to U) aren't listed -- nothing
// traps into U-mode-handled territory yet, so there's no delegation
// target below S to speak of.
constexpr uint16_t CSR_STVEC   = 0x105;
constexpr uint16_t CSR_SEPC    = 0x141;
constexpr uint16_t CSR_SCAUSE  = 0x142;
constexpr uint16_t CSR_STVAL   = 0x143;
constexpr uint16_t CSR_SATP    = 0x180; // must match mmu.cpp's own CSR_SATP

// Interrupt-related CSRs (Stage 2). mie/mip/sie/sip keep the base-spec bit
// positions unchanged -- AIA doesn't move them. miselect/siselect need no
// special handling below (they're just the plain selector value mireg/
// sireg read back out of Registers::csr[] each access), only mireg/sireg/
// mtopei/stopei do. mtopi/stopi are computed too (see compute_topi): they
// started out as generic-array zeros on the reasoning that nothing needed
// them, which turned out to be exactly wrong. A device tree advertising
// smaia/ssaia makes Linux dispatch interrupts solely from a csr_read of
// TOPI, so returning zero meant interrupts were never dispatched *or
// acknowledged* -- a silent livelock rather than a missing feature.
constexpr uint16_t CSR_SIE      = 0x104;
constexpr uint16_t CSR_SIP      = 0x144;
constexpr uint16_t CSR_MIE      = 0x304;
constexpr uint16_t CSR_MIP      = 0x344;
constexpr uint16_t CSR_MENVCFG  = 0x30A;
constexpr uint16_t CSR_STIMECMP = 0x14D; // Sstc, RV64 only (no stimecmph split)
constexpr uint16_t CSR_TIME     = 0xC01; // unprivileged, read-only mtime alias -- see the read-dispatch comment below
constexpr uint16_t CSR_MISELECT = 0x350;
constexpr uint16_t CSR_MIREG    = 0x351;
constexpr uint16_t CSR_SISELECT = 0x150;
constexpr uint16_t CSR_SIREG    = 0x151;
constexpr uint16_t CSR_MTOPEI   = 0x35C;
constexpr uint16_t CSR_STOPEI   = 0x15C;
constexpr uint16_t CSR_MTOPI    = 0xFB0; // Smaia top-interrupt, read-only
constexpr uint16_t CSR_STOPI    = 0xDB0; // Ssaia top-interrupt, read-only

constexpr uint64_t MIP_SSIP = 1ull << 1;
constexpr uint64_t MIP_MSIP = 1ull << 3;
constexpr uint64_t MIP_STIP = 1ull << 5;
constexpr uint64_t MIP_MTIP = 1ull << 7;
constexpr uint64_t MIP_SEIP = 1ull << 9;
constexpr uint64_t MIP_MEIP = 1ull << 11;
// What mip actually stores, raw, in Registers::csr[] -- a software-
// settable shadow for MSIP/SSIP (plain, always writable per spec),
// SEIP (spec explicitly allows a mode to inject a virtual S-level
// external interrupt this way, OR'd with the IMSIC's own signal below),
// and STIP (kept for a hypothetical SBI-style M-mode-managed timer,
// OR'd with the Sstc-derived condition). MTIP/MEIP have no shadow at
// all -- purely timer-derived and purely IMSIC-M-derived respectively,
// matching real hardware where M-mode's own sources are never
// software-injectable.
// Sscofpmf's LCOFI joins the software-settable shadow bits rather than
// being computed from the mhpmevent OF bits.
//
// That was worth getting wrong once to learn: the two are related but not
// the same signal. Hardware raises LCOFI at the *moment* a counter
// overflows, and it then stays pending until software clears it -- exactly
// like a device interrupt. Deriving it from OF instead would make it
// impossible for a handler to clear the interrupt without also clearing the
// overflow record it was about to read.
constexpr uint64_t MIP_LCOFIP = 1ull << 13;
constexpr uint64_t MIP_SHADOW_MASK = MIP_SSIP | MIP_MSIP | MIP_SEIP | MIP_STIP | MIP_LCOFIP;

// VS-level interrupts. These live in mip/mie alongside the M and S ones,
// and they are the mechanism by which a hypervisor makes a guest believe
// it has taken an interrupt: the hypervisor sets a bit in hvip, the bit
// appears in hip, hideleg routes it to VS-mode, and the guest sees an
// ordinary S-level interrupt.
//
// The cause the guest sees is one less than the bit number -- VSSIP (2)
// arrives as cause 1, VSTIP (6) as 5, VSEIP (10) as 9 -- because from
// inside the guest these *are* its supervisor software, timer and
// external interrupts. That renumbering is what makes a guest kernel run
// unmodified.
constexpr uint64_t MIP_VSSIP = 1ull << 2;
constexpr uint64_t MIP_VSTIP = 1ull << 6;
constexpr uint64_t MIP_VSEIP = 1ull << 10;
constexpr uint64_t MIP_SGEIP = 1ull << 12;
constexpr uint64_t HIP_MASK  = MIP_VSSIP | MIP_VSTIP | MIP_VSEIP | MIP_SGEIP;

// Only VSSIP is software-writable through hvip; VSTIP and VSEIP are
// read-only there because they are also driven by hardware -- the guest's
// timer and the guest external-interrupt file -- and hvip contributes to
// them by OR rather than by assignment.
constexpr uint64_t HVIP_WMASK = MIP_VSSIP | MIP_VSTIP | MIP_VSEIP;

constexpr uint16_t CSR_HIE  = 0x604;
constexpr uint16_t CSR_HIP  = 0x644;
constexpr uint16_t CSR_HVIP = 0x645;
constexpr uint16_t CSR_HGEIP = 0xE12;
constexpr uint16_t CSR_HGEIE = 0x607;
constexpr uint16_t CSR_HSTATUS_N = 0x600;

constexpr uint64_t MENVCFG_STCE = 1ull << 63;

// mstatus's virtualisation fields. Both sit above bit 32, so they exist
// only on RV64 -- MPV records whether an M-mode trap came from a virtual
// mode, and GVA whether the faulting address was a guest virtual one.
constexpr uint64_t MSTATUS_GVA = 1ull << 38;
constexpr uint64_t MSTATUS_MPV = 1ull << 39;

// Whether a trap's tval holds a guest *virtual* address, which is what
// hstatus.GVA and mstatus.GVA report. True for the faults whose tval is an
// address in the guest's own address space; false for ECALL, breakpoint
// and illegal instruction, whose tval is not an address at all.
//
// Both trap paths use this so they cannot drift apart: a guest fault that
// is delegated reports GVA in hstatus, and the same fault left undelegated
// reports it in mstatus, and a hypervisor reading either has to see the
// same answer.
inline bool tval_is_guest_va(uint64_t cause_bit, bool is_interrupt)
{
	if (is_interrupt) return false;
	switch (cause_bit) {
	case 0: case 1:            // instruction misaligned / access fault
	case 4: case 5:            // load misaligned / access fault
	case 6: case 7:            // store/AMO misaligned / access fault
	case 12: case 13: case 15: // page faults
	case 20: case 21: case 23: // guest-page faults
		return true;
	default:
		return false;
	}
}

constexpr uint64_t CAUSE_S_EXTERNAL = 9;
constexpr uint64_t CAUSE_S_TIMER    = 5;
constexpr uint64_t CAUSE_S_SOFTWARE = 1;
constexpr uint64_t CAUSE_M_EXTERNAL = 11;
constexpr uint64_t CAUSE_M_TIMER    = 7;
constexpr uint64_t CAUSE_M_SOFTWARE = 3;

// Sstc: STIP only reflects mtime>=stimecmp once menvcfg.STCE is set --
// spec-required gating, not an extra (see the Sstc 1.0 spec, "when STCE
// in menvcfg is zero... STIP... reverts to its defined behavior as if
// this extension is not implemented").
bool stip_from_sstc(Registers &regs, Memory &mem)
{
	if (!(regs.read_csr(CSR_MENVCFG) & MENVCFG_STCE)) return false;
	return mem.get_timer().get_mtime() >= regs.read_csr(CSR_STIMECMP);
}

uint64_t compute_mip(Registers &regs, Memory &mem)
{
	uint64_t raw = regs.read_csr(CSR_MIP) & MIP_SHADOW_MASK;
	uint64_t mip = raw & (MIP_MSIP | MIP_SSIP);

	if (mem.get_timer().mtip_pending()) mip |= MIP_MTIP;
	if ((raw & MIP_STIP) || stip_from_sstc(regs, mem)) mip |= MIP_STIP;
	if (mem.get_imsic_m().aggregate_pending()) mip |= MIP_MEIP;
	if ((raw & MIP_SEIP) || mem.get_imsic_s().aggregate_pending()) mip |= MIP_SEIP;

	if (Extensions.H) {
		// hvip is the hypervisor's injection register: whatever it sets
		// here is pending for the guest. VSTIP and VSEIP additionally
		// take hardware sources, so they are an OR rather than a copy --
		// a hypervisor clearing hvip.VSEIP must not clear an interrupt
		// the guest's external interrupt file is genuinely asserting.
		uint64_t hvip = regs.read_csr(CSR_HVIP) & HVIP_WMASK;
		mip |= hvip;

		// SGEIP is not injectable at all: it is the OR of the guest
		// external interrupts the hypervisor has enabled, and says "one
		// of your guests wants attention".
		if (regs.read_csr(CSR_HGEIP) & regs.read_csr(CSR_HGEIE)) mip |= MIP_SGEIP;

		// hstatus.VGEIN selects which guest external interrupt belongs to
		// the guest currently scheduled; that one, if pending, is the
		// guest's own VSEIP.
		uint64_t vgein = (regs.read_csr(CSR_HSTATUS_N) >> 12) & 0x3F;
		if (vgein != 0 && (regs.read_csr(CSR_HGEIP) & (1ull << vgein)))
			mip |= MIP_VSEIP;
	}
	return mip;
}

// Smaia/Ssaia mtopi/stopi: the highest-priority interrupt that is both
// pending and enabled for the given privilege level, encoded as
// {IID[27:16], IPRIO[7:0]}, or 0 when there is none.
//
// This is not optional decoration once the DT advertises smaia/ssaia:
// Linux's irq-riscv-intc then installs riscv_intc_aia_irq, whose entire
// body is `while ((topi = csr_read(CSR_TOPI))) generic_handle_domain_irq(
// intc_domain, topi >> TOPI_IID_SHIFT);`. With stopi reading 0 the loop
// never runs, so the interrupt is never dispatched *or* acknowledged --
// STIP stays asserted (only the timer handler re-arms stimecmp), the hart
// immediately re-traps, and the kernel livelocks silently: still
// executing, never progressing, no illegal instruction to catch it.
//
// IPRIO is reported as 1 (the default when no priority has been
// programmed); Linux only consumes the IID field, but the spec defines
// a nonzero default priority and 0 would be indistinguishable from
// "no interrupt".
uint64_t compute_topi(Registers &regs, Memory &mem, bool s_level)
{
	uint64_t mideleg = regs.read_csr(CSR_MIDELEG);
	uint64_t candidates = compute_mip(regs, mem) & regs.read_csr(CSR_MIE);
	candidates &= s_level ? mideleg : ~mideleg;

	// AIA default major-interrupt priority order, high to low, within a
	// level: external, then software, then timer.
	static const int s_order[] = { (int)CAUSE_S_EXTERNAL, (int)CAUSE_S_SOFTWARE, (int)CAUSE_S_TIMER };
	static const int m_order[] = { (int)CAUSE_M_EXTERNAL, (int)CAUSE_M_SOFTWARE, (int)CAUSE_M_TIMER };
	const int *order = s_level ? s_order : m_order;

	for (int i = 0; i < 3; i++) {
		if (candidates & (1ull << order[i])) return ((uint64_t)order[i] << 16) | 1u;
	}
	return 0;
}
uint64_t read_sie(Registers &regs)
{
	return regs.read_csr(CSR_MIE) & regs.read_csr(CSR_MIDELEG);
}

void write_sie(Registers &regs, uint64_t value)
{
	uint64_t mideleg = regs.read_csr(CSR_MIDELEG);
	uint64_t mie = regs.read_csr(CSR_MIE);
	regs.write_csr(CSR_MIE, (mie & ~mideleg) | (value & mideleg));
}

uint64_t read_sip(Registers &regs, Memory &mem)
{
	return compute_mip(regs, mem) & regs.read_csr(CSR_MIDELEG);
}

void write_sip(Registers &regs, uint64_t value)
{
	uint64_t mideleg = regs.read_csr(CSR_MIDELEG) & MIP_SHADOW_MASK;
	uint64_t raw = regs.read_csr(CSR_MIP);
	regs.write_csr(CSR_MIP, ((raw & ~mideleg) | (value & mideleg)) & MIP_SHADOW_MASK);
}

constexpr uint64_t MSTATUS_SIE  = 1ull << 1;
constexpr uint64_t MSTATUS_MIE  = 1ull << 3;
constexpr uint64_t MSTATUS_SPIE = 1ull << 5;
constexpr uint64_t MSTATUS_MPIE = 1ull << 7;
constexpr uint64_t MSTATUS_SPP  = 1ull << 8;    // 1 bit: previous mode was S(1) or U(0)
constexpr uint64_t MSTATUS_MPP  = 3ull << 11;   // 2 bits: previous mode, PrivMode-encoded

constexpr uint64_t CAUSE_ECALL_FROM_U = 8;
constexpr uint64_t CAUSE_ECALL_FROM_S = 9;
constexpr uint64_t CAUSE_ECALL_FROM_M = 11;
constexpr uint64_t CAUSE_BREAKPOINT   = 3;
constexpr uint64_t CAUSE_ILLEGAL_INSN = 2;
// mstatus.MPRV: an M-mode load or store is performed as though at
// mstatus.MPP, using that mode's translation and permissions.
constexpr uint64_t MSTATUS_MPRV = 1ull << 17;
// mstatus.TVM: with this set, S-mode may neither execute SFENCE.VMA nor
// touch satp -- both become illegal instructions, so a hypervisor sees
// every attempt a guest supervisor makes to manage its own translation.
constexpr uint64_t MSTATUS_TVM = 1ull << 20;

// sstatus is architecturally just the bits of mstatus a lower-privileged
// mode is allowed to see/touch -- SUM/MXR are read-write pass-through,
// SIE/SPIE/SPP alias the same-named mstatus bits directly.
// sstatus is a *view* of mstatus, and the view is wider than the
// interrupt-and-privilege bits it started as. It also carries:
//
//   FS  (14:13) and VS (10:9) -- the floating-point and vector state.
//       A supervisor decides whether to save those register files on a
//       context switch by reading them *here*; it has no access to
//       mstatus. Masking them out told every supervisor that no
//       extension state was ever live.
//   UXL (33:32) -- the XLEN U-mode runs at, read-only 2 (64-bit) here.
//   SD  (63)    -- the summary of FS/VS, supplied by vcommon::with_sd.
//
// Found by tracing: an arch-test trap handler reads sstatus and extracts
// bits 16:0 to rebuild a PTE. Sail read 0x...6600 (FS=3, VS=3), DoomV
// read 0, and the handler stored a PTE with its physical page number
// zeroed -- a difference that looked like a page-table bug and was a CSR
// masking bug three steps upstream.
constexpr uint64_t SSTATUS_FS  = 3ull << 13;
constexpr uint64_t SSTATUS_VS  = 3ull << 9;
constexpr uint64_t SSTATUS_UXL = 3ull << 32;
constexpr uint64_t SSTATUS_SD  = 1ull << 63;
constexpr uint64_t SSTATUS_MASK = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP
                                | (1ull << 18) | (1ull << 19)   // SUM, MXR
                                | SSTATUS_FS | SSTATUS_VS | SSTATUS_UXL | SSTATUS_SD;

// UXL and SD are read-only through this view: UXL because this hart has no
// 32-bit U-mode to switch to, SD because it is derived. A write must not
// reach either.
constexpr uint64_t SSTATUS_WMASK = SSTATUS_MASK & ~(SSTATUS_UXL | SSTATUS_SD);

// misa isn't plain csr[] storage -- it's computed fresh from Extensions on
// every read (WARL/hardwired: a write still lands in the generic array via
// the fallthrough below, but nothing ever reads that stored value back).
// This exists specifically because OpenSBI's sbi_init() calls
// misa_extension('S') to decide whether a hart is even eligible to become
// the coldboot hart for a next_mode==PRV_S jump -- with misa reading 0
// (its state before this existed), that check always fails, no hart ever
// wins the coldboot lottery, and hart 0 spins forever in
// init_warmboot's wait_for_coldboot(). S/U are set unconditionally (unlike
// I/M/A/C/F/D/V below): privilege modes have had no -march= toggle since
// Stage 1 (see registers.hpp's PrivMode), DoomV always supports them.
uint64_t compute_misa()
{
	uint64_t v = 0;
	auto bit = [&v](char c) { v |= 1ull << (c - 'A'); };
	if (Extensions.I) bit('I');
	if (Extensions.M) bit('M');
	if (Extensions.A) bit('A');
	if (Extensions.C) bit('C');
	if (Extensions.F) bit('F');
	if (Extensions.D) bit('D');
	if (Extensions.V) bit('V');
	if (Extensions.H) bit('H');
	bit('S');
	bit('U');
	v |= (Extensions.XLEN64 ? 2ull : 1ull) << (Extensions.XLEN64 ? 62 : 30);
	return v;
}

// satp.MODE is WARL (Write Any, Read Legal): real hardware that doesn't
// implement a given paging mode clamps an unsupported MODE write so a
// readback never reports support that isn't really there. mmu.cpp only
// implements MODE 0 (bare), 8 (Sv39), 9 (Sv48) and 10 (Sv57) -- everything
// else must not read back, since a write that sticks is how software
// discovers what a hart supports.
//
// Linux's own set_satp_mode() (arch/riscv/mm/init.c) relies on exactly
// this WARL behavior to autodetect paging depth: it writes a candidate
// satp (Sv57 first) and immediately reads it back via csr_swap -- if the
// value didn't stick, it falls back to Sv48 then Sv39. Without this
// rejection, DoomV always reported "yes, Sv57 stuck" (nothing was
// clamping it), so the kernel proceeded to actually run under Sv57 --
// which mmu_translate doesn't implement (it treats any non-Sv39,
// non-bare mode as raw identity passthrough), producing a garbage
// instruction fetch once the kernel started using its own high-half
// Sv57-style virtual addresses as if they were physical. Confirmed by
// bisecting crash.log: it halted on an illegal instruction at
// pc=0xffffffff80001146 (canonical high-half kernel VA) with
// satp.MODE=0xa (Sv57) already active.
void write_satp_warl(Registers &regs, uint16_t csr, uint64_t value)
{
	// 0 (Bare), 8 (Sv39), 9 (Sv48) and 10 (Sv57) are all implemented now,
	// so all four stick. Sv48 and Sv57 were rejected here for as long as
	// the walk only knew three levels -- rejecting them was the right
	// answer while that was true, because it is what made Linux's probe
	// fall back to a mode that worked. It is the wrong answer now: the
	// walk is written against the level count, the deeper modes translate,
	// and refusing them would report less than the hart can do.
	uint64_t mode = value >> 60;
	if (mode != 0 && mode != 8 && mode != 9 && mode != 10)
		return; // reject the whole write, not just the MODE field -- matches real WARL clamping
	regs.write_csr(csr, value);
}

void write_satp(Registers &regs, uint64_t value)
{
	write_satp_warl(regs, CSR_SATP, value);
}

// hgatp's MODE is WARL on the same terms as satp's, with its own set of
// legal values: 0 (Bare), 8 (Sv39x4), 9 (Sv48x4), 10 (Sv57x4). Everything
// else is reserved, and a reserved value must not read back -- software
// probes the field by writing a candidate and seeing what survives, so
// storing 2 tells the prober this hart implements a second-stage mode it
// has never heard of.
//
// PPN[1:0] additionally read as zero: the root of a G-stage table is
// 16KiB-aligned, not 4KiB, because the top level is four pages wide.
void write_hgatp_warl(Registers &regs, uint64_t value)
{
	uint64_t mode = value >> 60;
	if (mode != 0 && mode != 8 && mode != 9 && mode != 10) return;
	regs.write_csr(hyp::CSR_HGATP_ADDR, value & ~0x3ull);
}

uint64_t read_sstatus(Registers &regs)
{
	// SD is part of sstatus's view too, and is derived rather than stored
	// -- see vcommon::with_sd.
	// UXL is read-only 2: this hart's U-mode is always 64-bit.
	return (vcommon::with_sd(regs.read_csr(CSR_MSTATUS)) & SSTATUS_MASK)
	     | (2ull << 32);
}

void write_sstatus(Registers &regs, uint64_t value)
{
	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
	mstatus = (mstatus & ~SSTATUS_WMASK) | (value & SSTATUS_WMASK);
	regs.write_csr(CSR_MSTATUS, mstatus);
}
}

// What a CSR read actually returns -- shared by exec_32ZICSR's real read
// side and anything else that just wants to *peek* a live value (the
// dashboard's CSRs panel, via DoomSystem::publish_snapshot). Several CSRs
// are computed, not plain csr[] storage: sstatus is a masked view of
// mstatus; mip/sip fold in the timer + IMSIC aggregate; misa is computed
// fresh from Extensions (Stage 3, see compute_misa's own comment); time
// is a read-only mtime alias (Stage 4); mireg/sireg/mtopei/stopei read
// through to the IMSIC files owned by Memory; fflags/frm/fcsr and V's
// vstart/vxsat/vxrm/vl/vtype/vlenb live in Registers' own dedicated
// fields, not csr[], for the same OR-accumulate/read-only-in-practice
// reasons noted at each accessor's declaration (registers.hpp). Side-
// effect free either way -- topei_value() is a plain peek; claim() is a
// separate call the real write side makes only on an actual write.
// Who is allowed to touch a CSR. Three rules, all from the privileged spec's
// CSR-address encoding, plus the counter chain Zicntr/Zihpm add:
//
//   * csr[11:10] == 11 marks a read-only CSR -- writing one is illegal.
//   * csr[9:8] is the lowest privilege that may access it at all.
//   * the unprivileged counters are further gated by mcounteren/scounteren.
//
// None of this was enforced before: every mode could read and write every
// CSR. That is invisible while only M-mode firmware runs, and becomes very
// visible the moment a guest kernel deliberately probes a CSR expecting a
// trap -- which is exactly how OpenSBI detects hart features.
//
// Deliberately *not* added here: trapping on CSR numbers this machine gives
// no meaning to. Registers::csr[] backs all 4096 addresses generically, and
// OpenSBI's feature detection reads a spread of them to see which exist.
// Making unknown CSRs illegal is a separate, much larger behaviour change
// than making privilege boundaries real, and belongs in its own step.
bool RiscvCore::csr_access_permitted(Registers &regs, uint16_t csr, bool writing)
{
	if (writing && ((csr >> 10) & 0x3) == 0x3) return false;

	// csr[9:8] normally encodes the lowest privilege that may access the
	// register -- 0 for U, 1 for S, 3 for M. The value 2 is not a
	// privilege level at all: it is the hypervisor and VS-CSR encoding,
	// and those registers are reachable from HS-mode and M.
	//
	// Reading it as a literal privilege number denies every VS CSR to
	// every mode, since no PrivMode equals 2. That is invisible until
	// something actually redirects an S-mode CSR name into the 0x2xx
	// range, at which point a guest's ordinary csrw stvec starts trapping.
	uint8_t min_priv = (csr >> 8) & 0x3;
	if (min_priv == 2) min_priv = (uint8_t)PrivMode::S;
	if ((uint8_t)regs.get_priv() < min_priv) return false;

	if (counters::is_counter_csr(csr) && !counters::counter_permitted(regs, csr))
		return false;

	// mstatus.TVM closes satp to S-mode, reads included. Together with the
	// SFENCE.VMA trap it gives a hypervisor a complete view of a guest
	// supervisor's attempts to manage translation: it cannot install a root
	// table, and it cannot read back the one it is running under.
	// hgatp is closed by the same bit and for the same reason: it is the
	// hypervisor's own second-stage root, and M-mode withholding
	// translation control has to withhold all of it.
	if ((csr == CSR_SATP || (Extensions.H && csr == hyp::CSR_HGATP_ADDR))
	    && regs.get_priv() == PrivMode::S
	    && (regs.read_csr(CSR_MSTATUS) & MSTATUS_TVM))
		return false;

	// The stateen registers gate each other down the privilege hierarchy:
	// mstateen's SE0 bit controls whether sstateen and hstateen are
	// reachable at all from below M, and hstateen's controls whether a
	// guest may reach sstateen. Denying at the top denies all the way
	// down, which is what lets a hypervisor withhold state it does not
	// understand well enough to context-switch.
	if (Extensions.SSSTATEEN && stateen::is_stateen_csr(csr)
	    && !stateen::stateen_access_permitted(regs, csr))
		return false;

	return true;
}


// henvcfg's bits are not independent of menvcfg's. Three of them name an
// extension M-mode can withhold, and withholding at the top has to
// withhold all the way down: if menvcfg.STCE is clear, henvcfg.STCE is
// read-only zero, and the same for PBMTE and ADUE. Without that, a
// hypervisor could hand a guest an extension the machine had switched
// off, and -- worse for anything probing -- henvcfg would read back a
// capability that does not work.
//
// The masking has to apply on *read* as well as on write. A bit set while
// menvcfg permitted it must disappear the moment menvcfg is cleared,
// rather than staying visible until something writes henvcfg again.
//
// CBIE, CBZE, CBCFE, LPE, SSE and FIOM are deliberately absent: those are
// per-mode controls, not delegated capabilities, and a guest's setting is
// its own. One of the hypervisor tests checks exactly that, asserting that
// VS-mode's LPE is independent of menvcfg.LPE.
uint64_t henvcfg_mask(Registers &regs)
{
	constexpr uint64_t DELEGATED = (1ull << 63)   // STCE
	                             | (1ull << 62)   // PBMTE
	                             | (1ull << 61);  // ADUE
	return ~DELEGATED | regs.read_csr(CSR_MENVCFG);
}

// CBIE (bits 5:4) selects what cbo.inval does: 0 traps, 1 flushes, 3
// invalidates. 2 is reserved, and WARL means a reserved value must not
// read back -- software probes the field by writing a value and seeing
// what survives, so retaining 2 claims a behaviour this hart does not
// have. The ordinary WARL response is to keep the field it had.
uint64_t cbie_warl(uint64_t updated, uint64_t old)
{
	constexpr uint64_t CBIE = 3ull << 4;
	if (((updated >> 4) & 0x3) == 2) return (updated & ~CBIE) | (old & CBIE);
	return updated;
}


// hip and hie are HS-mode's windows onto the VS-level bits of mip and mie.
// They are views, not storage: hip.VSSIP *is* mip.VSSIP, and writing it
// writes hvip, which is what makes the read and write directions agree.
// Holding them as separate registers is the mistake that makes an
// injected interrupt visible in hvip and nowhere else.
uint64_t read_hip(Registers &regs, Memory &mem)
{
	return compute_mip(regs, mem) & HIP_MASK;
}

void write_hip(Registers &regs, uint64_t value)
{
	// VSSIP alone is writable here, and the write lands in hvip. VSTIP,
	// VSEIP and SGEIP are read-only through hip: they are asserted by
	// hardware, and a hypervisor that wants to inject them does so
	// through hvip instead.
	uint64_t hvip = regs.read_csr(CSR_HVIP);
	regs.write_csr(CSR_HVIP, (hvip & ~MIP_VSSIP) | (value & MIP_VSSIP));
}

uint64_t read_hie(Registers &regs)
{
	return regs.read_csr(CSR_MIE) & HIP_MASK;
}

void write_hie(Registers &regs, uint64_t value)
{
	uint64_t mie = regs.read_csr(CSR_MIE);
	regs.write_csr(CSR_MIE, (mie & ~HIP_MASK) | (value & HIP_MASK));
}

void write_hvip(Registers &regs, uint64_t value)
{
	regs.write_csr(CSR_HVIP, value & HVIP_WMASK);
}


// mideleg's VS-level bits are read-only 1 when the hypervisor extension is
// implemented. VS interrupts have nowhere else to go: they exist to be
// handled by HS-mode or delegated onward to the guest, and M-mode taking
// them directly would mean the machine servicing an interrupt raised for a
// guest it knows nothing about. Making them writable lets software clear a
// bit and then wait forever for an interrupt that is no longer routed
// anywhere.
//
// Bit 12 (SGEIP) is *not* in this set, because GEILEN is zero on this hart
// -- there is no guest external interrupt controller, so the interrupt it
// delegates does not exist. That is a configuration property, not a gap:
// hgeie and hgeip read as zero for the same reason, and software that
// probes them discovers GEILEN=0 and stops asking.
uint64_t mideleg_fixed_ones()
{
	if (!Extensions.H) return 0;
	return MIP_VSSIP | MIP_VSTIP | MIP_VSEIP;
}

uint64_t RiscvCore::read_csr_effective(Registers &regs, Memory &mem, uint16_t csr)
{
	// PMP entries past the implemented count read as zero rather than as
	// whatever was last written to an unimplemented register.
	if (csr == CSR_MSTATUS) return vcommon::with_sd(regs.read_csr(CSR_MSTATUS));
	if (pmp::is_pmpcfg(csr)) return pmp::read_cfg(regs, csr);
	if (pmp::is_pmpaddr(csr)) return pmp::read_addr(regs, csr);
	if (csr == 0x100) return read_sstatus(regs);
	if (csr == CSR_MISA) return compute_misa();
	if (Extensions.H && csr == hyp::CSR_HSTATUS_ADDR) return hyp::read_hstatus(regs);
	if (Extensions.H && csr == 0x60A) return regs.read_csr(0x60A) & henvcfg_mask(regs);
	if (csr == CSR_MIDELEG) return regs.read_csr(CSR_MIDELEG) | mideleg_fixed_ones();
	// GEILEN is zero: no guest external interrupt file exists, so both
	// registers that describe one read as zero however they were written.
	if (Extensions.H && (csr == CSR_HGEIE || csr == CSR_HGEIP)) return 0;
	if (Extensions.H && csr == CSR_HIP) return read_hip(regs, mem);
	if (Extensions.H && csr == CSR_HIE) return read_hie(regs);
	if (Extensions.H && csr == CSR_HVIP) return regs.read_csr(CSR_HVIP) & HVIP_WMASK;
	if (csr == CSR_SIE) return read_sie(regs);
	if (csr == CSR_SIP) return read_sip(regs, mem);
	if (csr == CSR_MIP) return compute_mip(regs, mem);
	if (csr == CSR_MIREG) return mem.get_imsic_m().read_indirect(regs.read_csr(CSR_MISELECT));
	if (csr == CSR_SIREG) return mem.get_imsic_s().read_indirect(regs.read_csr(CSR_SISELECT));
	if (csr == CSR_MTOPEI) return mem.get_imsic_m().topei_value();
	if (csr == CSR_STOPEI) return mem.get_imsic_s().topei_value();
	if (csr == CSR_MTOPI) return compute_topi(regs, mem, /*s_level=*/false);
	if (csr == CSR_STOPI) return compute_topi(regs, mem, /*s_level=*/true);
	// cycle/time/instret/hpmcounter* (Zicntr, Zihpm). time was already
	// here as an mtime alias; the other two read the same counter for the
	// reason ext_zicntr.cpp explains.
	if (counters::is_counter_csr(csr)) return counters::read_counter(regs, mem, csr);
	// scountovf has no storage of its own -- it is assembled from the OF
	// bits of every mhpmevent, so the two cannot drift apart.
	if (Extensions.SSCOFPMF && csr == sscofpmf::CSR_SCOUNTOVF) return sscofpmf::read_scountovf(regs);
	if (Extensions.SSSTATEEN && stateen::is_stateen_csr(csr)) return stateen::read_stateen(regs, csr);
	if (csr == 0x001) return regs.get_fflags();
	if (csr == 0x002) return regs.get_frm();
	if (csr == 0x003) return ((uint64_t)regs.get_frm() << 5) | regs.get_fflags();
	if (csr == 0x008) return regs.get_vstart();
	if (csr == 0x009) return regs.get_vxsat();
	if (csr == 0x00A) return regs.get_vxrm();
	if (csr == 0x00F) return ((uint64_t)regs.get_vxrm() << 1) | regs.get_vxsat();
	if (csr == 0xC20) return regs.get_vl();
	if (csr == 0xC21) return regs.get_vtype();
	if (csr == 0xC22) return Registers::VLEN_BYTES;
	return regs.read_csr(csr);
}

bool RiscvCore::translate_or_trap(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type, uint64_t &paddr, unsigned size)
{
	uint64_t cause, tval;
	if (!mmu_translate(regs, mem, vaddr, type, paddr, cause, tval)) {
		enter_trap(regs, cause, tval);
		return false;
	}

	// PMP is checked on the *physical* address, after translation, and a
	// denial is an access fault rather than a page fault. The difference
	// is not cosmetic: a page fault tells the supervisor to fix a mapping
	// and retry, while an access fault says this physical region is not
	// reachable at this privilege however the tables are arranged.
	//
	// The privilege used is mstatus.MPP when MPRV is set and the access is
	// a load or store, matching the translation the MMU just did. Checking
	// at the current mode instead would let an M-mode MPRV access reach
	// memory the effective privilege is denied.
	static constexpr uint64_t CAUSE_INST_ACCESS_F  = 1;
	static constexpr uint64_t CAUSE_LOAD_ACCESS_F  = 5;
	static constexpr uint64_t CAUSE_STORE_ACCESS_F = 7;
	auto access_cause = [&](AccessType t) {
		if (t == AccessType::Fetch) return CAUSE_INST_ACCESS_F;
		if (t == AccessType::Load)  return CAUSE_LOAD_ACCESS_F;
		return CAUSE_STORE_ACCESS_F;   // Store, Amo, CacheBlock
	};

	// Physical memory attributes come first: an address nothing answers is
	// an access fault regardless of what PMP would have said about it.
	if (!mem.is_backed(paddr, size)) {
		enter_trap(regs, access_cause(type), vaddr);
		return false;
	}

	if (Extensions.SMPMP) {
		uint8_t priv = (uint8_t)regs.get_priv();
		if (type != AccessType::Fetch) {
			uint64_t st = regs.read_csr(CSR_MSTATUS);
			if (st & MSTATUS_MPRV) priv = (uint8_t)((st >> 11) & 3);
		}
		// A cache-block operation is permitted by PMP on read *or* write,
		// the same rule its page permissions follow. Funnelling it into the
		// store check demanded write and faulted on a legitimately
		// read-only region -- over-faulting, which is just as wrong as
		// letting an access through and harder to notice, since a spurious
		// trap looks like the feature working.
		bool ok;
		if (type == AccessType::CacheBlock) {
			ok = pmp::check(regs, paddr, size, pmp::ACC_LOAD, priv)
			  || pmp::check(regs, paddr, size, pmp::ACC_STORE, priv);
		} else {
			int acc = (type == AccessType::Fetch) ? pmp::ACC_FETCH
			        : (type == AccessType::Load)  ? pmp::ACC_LOAD
			                                      : pmp::ACC_STORE;
			ok = pmp::check(regs, paddr, size, acc, priv);
		}
		if (!ok) {
			uint64_t c = access_cause(type);
			// tval is the faulting *virtual* address, as for a page fault.
			enter_trap(regs, c, vaddr);
			return false;
		}
	}
	return true;
}

void RiscvCore::raise_illegal_instruction(Registers &regs, uint64_t tval)
{
	// pc still sits on the offending instruction (a disabled/unknown
	// encoding is never executed, so nothing advanced it), which is exactly
	// what the trap should record as the return address.
	enter_trap(regs, CAUSE_ILLEGAL_INSN, tval);
}

void RiscvCore::enter_trap(Registers &regs, uint64_t cause, uint64_t tval, bool is_interrupt)
{
	uint64_t pc = regs.get_pc();
	PrivMode from = regs.get_priv();

	// Interrupt causes have bit 63 set (e.g. (1<<63)|7 for an M-timer
	// interrupt) -- strip it for the delegation-bit lookup, which always
	// indexes by the low cause number regardless. Interrupts delegate via
	// mideleg, exceptions via medeleg; either way, delegation only ever
	// applies below M (already-M-mode traps always stay in M) and only
	// downward, never back up to a mode the hart has already left.
	uint64_t cause_bit = cause & 0x7FFFFFFFFFFFFFFFull;
	uint64_t deleg = is_interrupt ? regs.read_csr(CSR_MIDELEG) : regs.read_csr(CSR_MEDELEG);
	bool to_s = (from != PrivMode::M) && (deleg & (1ull << cause_bit));

	// Whether the hart was virtual when the trap happened must be saved
	// before it is cleared -- into hstatus.SPV for a trap taken to HS, or
	// mstatus.MPV for one taken to M. Without it the eventual xRET has no
	// way to know it should resume a guest, and would return to the
	// hypervisor's privilege level still running the guest's code.
	bool was_virt = Extensions.H && regs.get_virt();

	// A trap from a guest can be delegated one step further. medeleg sends
	// it from M down to HS; hedeleg sends it from HS down to the guest's
	// own handler, so the guest kernel services its own page faults and
	// system calls without the hypervisor being involved at all. That is
	// what makes virtualisation cheap -- a hypervisor that had to mediate
	// every guest syscall would be unusable.
	//
	// Both levels have to agree: a cause the hypervisor has not delegated
	// stays with the hypervisor even if the guest would like it.
	uint64_t hdeleg = is_interrupt ? regs.read_csr(0x603)  // hideleg
	                               : regs.read_csr(0x602); // hedeleg
	bool to_vs = to_s && was_virt && (hdeleg & (1ull << cause_bit));

	if (to_vs) {
		// The guest's own trap registers, not the hypervisor's. These are
		// the vs* shadows -- which is also what the guest would reach by
		// their S-mode names, so from inside the guest this is
		// indistinguishable from taking a trap on real hardware.
		// A VS-level *interrupt* is renumbered on the way in: the guest
		// must see its own supervisor cause, not the VS-level one. VSSIP
		// (bit 2) arrives as cause 1, VSTIP (6) as 5, VSEIP (10) as 9 --
		// one less in every case, since the VS bits sit exactly one
		// position above the S bits they stand in for. Exceptions carry
		// their own numbers through unchanged.
		//
		// Without this a guest kernel reads cause 2 for a software
		// interrupt and dispatches on a number that means nothing to it.
		uint64_t vs_cause = cause;
		if (is_interrupt) vs_cause = (cause & (1ull << 63)) | (cause_bit - 1);

		regs.write_csr(0x241, pc);       // vsepc
		regs.write_csr(0x242, vs_cause); // vscause
		regs.write_csr(0x243, tval);     // vstval

		// Interrupt-enable stacking happens in vsstatus, the guest's own
		// sstatus. Using the real sstatus here would corrupt the
		// hypervisor's interrupt state on every guest trap.
		uint64_t vsstatus = regs.read_csr(0x200);
		vsstatus = (vsstatus & MSTATUS_SIE) ? (vsstatus | MSTATUS_SPIE) : (vsstatus & ~MSTATUS_SPIE);
		vsstatus &= ~MSTATUS_SIE;
		vsstatus = (from == PrivMode::S) ? (vsstatus | MSTATUS_SPP) : (vsstatus & ~MSTATUS_SPP);
		regs.write_csr(0x200, vsstatus);

		// The hart stays virtual: this trap never left the guest.
		regs.set_priv(PrivMode::S);
		regs.set_pc(regs.read_csr(0x205) & ~0x3ull); // vstvec
		return;
	}

	if (to_s) {
		regs.write_csr(CSR_SEPC, pc);
		regs.write_csr(CSR_SCAUSE, cause);
		regs.write_csr(CSR_STVAL, tval);

		uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
		mstatus = (mstatus & MSTATUS_SIE) ? (mstatus | MSTATUS_SPIE) : (mstatus & ~MSTATUS_SPIE);
		mstatus &= ~MSTATUS_SIE;
		mstatus = (from == PrivMode::S) ? (mstatus | MSTATUS_SPP) : (mstatus & ~MSTATUS_SPP);
		regs.write_csr(CSR_MSTATUS, mstatus);

		if (Extensions.H) {
			// htval is written by the MMU itself when a second-stage
			// walk fails -- it is the only code that knows the guest
			// physical address, and stval must keep the guest virtual
			// one. Here it is only cleared for the causes that have no
			// second-stage address to report, so a stale value from an
			// earlier fault cannot be mistaken for a fresh one.
			// htval carries the guest physical address of a G-stage
			// fault, shifted right by two, while stval keeps the guest
			// *virtual* one -- the hypervisor needs both, and one field
			// cannot serve for both. For any other cause there is no
			// second-stage address to report, and htval is cleared so a
			// stale value from an earlier fault cannot be mistaken for a
			// fresh one.
			bool has_gpa = !is_interrupt
			            && (cause_bit == 20 || cause_bit == 21 || cause_bit == 23);
			regs.write_csr(0x643, has_gpa ? (regs.pending_gpa >> 2) : 0);

			// A trap from a guest into HS-mode leaves virtual mode.
			// SPVP records the guest's own privilege, so the
			// hypervisor can tell it interrupted VS rather than VU.
			uint64_t hstatus = hyp::read_hstatus(regs);
			hstatus = was_virt ? (hstatus | (1ull << 7)) : (hstatus & ~(1ull << 7)); // SPV
			if (was_virt) {
				hstatus = (from == PrivMode::S) ? (hstatus | (1ull << 8))
				                                : (hstatus & ~(1ull << 8)); // SPVP
			}

			// GVA says whether stval holds a guest *virtual* address. The
			// hypervisor needs it to know how to read stval at all: for a
			// fault it is an address in the guest's own address space and
			// means nothing without the guest's page tables, while for an
			// ECALL or an illegal instruction stval is not an address and
			// GVA must read zero.
			//
			// It is written on every trap to HS, not only when set: leaving
			// it alone would let a stale 1 from an earlier fault make the
			// hypervisor read a non-address as a guest pointer.
			static constexpr uint64_t HSTATUS_GVA_BIT = 1ull << 6;
			bool gva = was_virt && tval_is_guest_va(cause_bit, is_interrupt);
			hstatus = gva ? (hstatus | HSTATUS_GVA_BIT) : (hstatus & ~HSTATUS_GVA_BIT);

			hyp::write_hstatus(regs, hstatus);
			regs.set_virt(false);
		}
		regs.set_priv(PrivMode::S);
		// Direct mode only (stvec[1:0] ignored) -- vectored mode's
		// cause-indexed offset isn't implemented; every trap, interrupt or
		// not, goes to the same base address.
		regs.set_pc(regs.read_csr(CSR_STVEC) & ~0x3ull);
		return;
	}

	regs.write_csr(CSR_MEPC, pc);
	regs.write_csr(CSR_MCAUSE, cause);
	regs.write_csr(CSR_MTVAL, tval);

	// Standard M-mode enable stacking: the current interrupt-enable bit is
	// saved to MPIE and cleared, so a handler doesn't get pre-empted by
	// itself; MRET reverses this. MPP records the mode being trapped out
	// of, same idea as SPP above but 2 bits since M-mode traps can be
	// entered from any of the three modes.
	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
	mstatus = (mstatus & MSTATUS_MIE) ? (mstatus | MSTATUS_MPIE) : (mstatus & ~MSTATUS_MPIE);
	mstatus &= ~MSTATUS_MIE;
	mstatus = (mstatus & ~MSTATUS_MPP) | (((uint64_t)from & 0x3) << 11);
	// MPV is the M-mode counterpart of hstatus.SPV: it is what tells the
	// eventual MRET whether the mode it is returning to was virtual.
	if (Extensions.H) {
		mstatus = was_virt ? (mstatus | MSTATUS_MPV) : (mstatus & ~MSTATUS_MPV);
		// GVA has an M-mode copy for exactly the same reason it has an
		// HS-mode one, and it was defined here and never written. A guest
		// fault that is not delegated lands in M with mtval holding a guest
		// virtual address, and nothing said so.
		bool mgva = was_virt && tval_is_guest_va(cause_bit, is_interrupt);
		mstatus = mgva ? (mstatus | MSTATUS_GVA) : (mstatus & ~MSTATUS_GVA);
	}
	regs.write_csr(CSR_MSTATUS, mstatus);

	if (Extensions.H) {
		// mtval2 is htval's M-mode counterpart, and it was never written.
		// A guest page fault that the hypervisor has not been delegated
		// lands here, and firmware that forwards such a trap -- by copying
		// mtval2 into htval and entering the HS handler -- was copying a
		// zero over the only correct value the fault had produced.
		bool m_has_gpa = !is_interrupt
		              && (cause_bit == 20 || cause_bit == 21 || cause_bit == 23);
		regs.write_csr(0x34B, m_has_gpa ? (regs.pending_gpa >> 2) : 0);
	}
	if (Extensions.H) regs.set_virt(false); // M-mode is never virtual
	regs.set_priv(PrivMode::M);
	// Direct mode only (mtvec[1:0] ignored) -- vectored mode's cause-indexed
	// offset for interrupts isn't implemented; every trap goes to the same
	// base address regardless of mode or cause.
	regs.set_pc(regs.read_csr(CSR_MTVEC) & ~0x3ull);
}

bool RiscvCore::check_and_take_interrupt(Registers &regs, Memory &mem)
{
	uint64_t pending_enabled = compute_mip(regs, mem) & regs.read_csr(CSR_MIE);
	if (!pending_enabled) return false;

	// Fixed priority order per spec: MEI > MSI > MTI > SEI > SSI > STI,
	// then the VS-level ones below those, then SGEI. A VS interrupt is
	// lower priority than every interrupt belonging to a more privileged
	// mode, which is the whole point -- the hypervisor gets to run before
	// the guest it is injecting into.
	static const int priority_order[] = {
		(int)CAUSE_M_EXTERNAL, (int)CAUSE_M_SOFTWARE, (int)CAUSE_M_TIMER,
		(int)CAUSE_S_EXTERNAL, (int)CAUSE_S_SOFTWARE, (int)CAUSE_S_TIMER,
		12 /* SGEI */, 10 /* VSEI */, 2 /* VSSI */, 6 /* VSTI */,
	};
	int bit = -1;
	for (int b : priority_order) {
		if (pending_enabled & (1ull << b)) { bit = b; break; }
	}
	if (bit < 0) return false; // only a bit outside this fixed set is pending -- nothing this project defines yet

	PrivMode priv = regs.get_priv();
	bool to_s = (regs.read_csr(CSR_MIDELEG) & (1ull << bit)) != 0;
	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);

	// A VS-level interrupt that HS-mode has delegated onward with hideleg
	// belongs to the guest, and can only be taken while the guest is
	// actually running. Its enable is the guest's own vsstatus.SIE, not
	// the hypervisor's sstatus.SIE: a hypervisor with interrupts disabled
	// does not thereby disable its guest's.
	//
	// enter_trap handles the renumbering (VSSIP's bit 2 arrives as cause
	// 1) and the vs* register selection, so nothing here has to know
	// about it -- this only decides whether the interrupt is taken.
	if (Extensions.H && to_s && (regs.read_csr(0x603) & (1ull << bit))) {
		if (!regs.get_virt()) return false;   // no guest running to take it
		if (priv == PrivMode::S && !(regs.read_csr(0x200) & MSTATUS_SIE))
			return false;                      // vsstatus.SIE
		enter_trap(regs, (1ull << 63) | (uint64_t)bit, 0, /*is_interrupt=*/true);
		return true;
	}

	if (!to_s) {
		// M-target: always taken from S/U; from M itself only if MIE is
		// set (a hart in M can mask its own interrupts, but a mode below
		// M can never mask one that isn't delegated to it).
		if (priv == PrivMode::M && !(mstatus & MSTATUS_MIE)) return false;
	} else {
		// S-target: always taken from U; from S itself only if SIE is
		// set; never taken while already in M (M can't be pre-empted by
		// a trap delegated to a less-privileged mode).
		if (priv == PrivMode::M) return false;
		if (priv == PrivMode::S && !(mstatus & MSTATUS_SIE)) return false;
	}

	enter_trap(regs, (1ull << 63) | (uint64_t)bit, 0, /*is_interrupt=*/true);
	return true;
}

void RiscvCore::exec_32ZICSR(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	if (instr.funct3 == 0) {
		// SFENCE.VMA (funct7==0b0001001, rd==0) isn't distinguishable by
		// instr.imm alone -- rs2 (the ASID operand) varies per encoding, so
		// it's matched on funct7 directly, ahead of the fixed-immediate
		// switch below. No TLB exists to flush yet, so this is a real,
		// deliberate no-op rather than an unrecognized encoding.
		if (instr.funct7 == 0b0001001 && instr.rd == 0) {
			// SFENCE.VMA is a supervisor instruction: attempting it from
			// U-mode is illegal regardless of TVM, and DoomV let it through.
			if (regs.get_priv() == PrivMode::U) {
				raise_illegal_instruction(regs, instr.raw);
				return;
			}

			// mstatus.TVM makes SFENCE.VMA illegal in S-mode. The point is
			// not the fence -- there is no TLB here to flush -- but that a
			// hypervisor running a guest supervisor traps on it to know the
			// guest touched its page tables. A hart that quietly succeeds
			// tells the hypervisor nothing happened.
			// In VS-mode the governing bit is hstatus.VTVM, and the trap is
			// a virtual instruction rather than an illegal one -- same
			// reasoning as the satp case above.
			if (Extensions.H && regs.get_virt() && regs.get_priv() == PrivMode::S
			    && (regs.read_csr(hyp::CSR_HSTATUS_ADDR) & (1ull << 20))) {
				enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
				return;
			}
			if (!regs.get_virt() && regs.get_priv() == PrivMode::S
			    && (regs.read_csr(CSR_MSTATUS) & MSTATUS_TVM)) {
				raise_illegal_instruction(regs, instr.raw);
				return;
			}
			regs.set_pc(pc + instr.length);
			return;
		}

		// ECALL/EBREAK/MRET/SRET/WFI -- control transfer, not a CSR
		// read/modify/write. Illegal-instruction detection deliberately
		// stays a separate, unconditional debugger halt (see
		// DoomSystem::step) rather than a real trap here: this project
		// still has no illegal-instruction trap handler set up anywhere
		// (OpenSBI/a kernel will eventually provide one), so routing
		// illegal instructions through this same path would just spin
		// forever re-trapping instead of surfacing a crash log.
		switch (instr.imm) {
		case 0x000: { // ECALL -- cause depends on the mode making the call
			PrivMode priv = regs.get_priv();
			uint64_t cause = (priv == PrivMode::M) ? CAUSE_ECALL_FROM_M
			                : (priv == PrivMode::S) ? CAUSE_ECALL_FROM_S
			                                         : CAUSE_ECALL_FROM_U;
			// A guest's ecall gets its own cause, 10, distinct from
			// HS-mode's 9. That is how a hypervisor tells a call from
			// the guest kernel apart from one made by its own
			// supervisor code -- the two mean entirely different
			// things and are handled by different code paths. VU-mode
			// keeps cause 8, the same as U: the guest's userspace
			// calls its own kernel, not the hypervisor.
			if (Extensions.H && regs.get_virt() && regs.get_priv() == PrivMode::S)
				cause = 10;
			enter_trap(regs, cause, 0);
			return;
		}
		case 0x001: // EBREAK
			enter_trap(regs, CAUSE_BREAKPOINT, pc);
			return;
		case 0x102: { // SRET -- mirrors MRET below, using the S-mode fields
			// hstatus.VTSR traps a guest supervisor's SRET as a *virtual*
			// instruction (cause 22) rather than an illegal one, so the
			// hypervisor can emulate the return itself. The bit was defined
			// and made writable and then never consulted, so a guest with
			// VTSR set simply returned -- to whatever sepc held, which in a
			// test that never set it is zero.
			//
			// Cause 22 and not 2: the distinction is the whole point. An
			// illegal instruction tells the guest it did something no one
			// may do; a virtual instruction tells the hypervisor the guest
			// did something only the hypervisor may do, and can be emulated
			// on its behalf.
			if (Extensions.H && regs.get_virt() && regs.get_priv() == PrivMode::S
			    && (regs.read_csr(hyp::CSR_HSTATUS_ADDR) & (1ull << 22))) {
				enter_trap(regs, 22, 0);
				return;
			}
			// In VS-mode the names sstatus and sepc mean the guest's own
			// vsstatus and vsepc -- the same redirection every other S-mode
			// CSR access already goes through. SRET was reading the
			// hypervisor's copies instead, so a guest returning from its own
			// trap resumed at the *hypervisor's* sepc, which in a test that
			// never set one is zero.
			const bool vs = Extensions.H && regs.get_virt();
			const uint16_t status_csr = vs ? hyp::CSR_VSSTATUS_ADDR : CSR_MSTATUS;
			const uint16_t epc_csr    = vs ? hyp::CSR_VSEPC_ADDR    : CSR_SEPC;

			uint64_t mstatus = regs.read_csr(status_csr);
			mstatus = (mstatus & MSTATUS_SPIE) ? (mstatus | MSTATUS_SIE) : (mstatus & ~MSTATUS_SIE);
			mstatus |= MSTATUS_SPIE; // SPIE reset to 1 on return, per spec
			PrivMode target = (mstatus & MSTATUS_SPP) ? PrivMode::S : PrivMode::U;
			mstatus &= ~MSTATUS_SPP; // SPP reset to U on return, per spec
			regs.write_csr(status_csr, mstatus);

			// An SRET in HS-mode returns to the guest when hstatus.SPV
			// says the trap came from one; SPV is then cleared, so a
			// later SRET cannot accidentally resume in VS-mode without
			// a guest having been entered again.
			//
			// An SRET in VS-mode is the guest's own return from its own
			// trap. It stays virtual, and hstatus -- which belongs to
			// the hypervisor, not the guest -- is not consulted.
			if (Extensions.H && !regs.get_virt()) {
				uint64_t hstatus = hyp::read_hstatus(regs);
				regs.set_virt((hstatus & (1ull << 7)) != 0); // SPV
				hyp::write_hstatus(regs, hstatus & ~(1ull << 7));
			}
			regs.set_priv(target);
			regs.set_pc(regs.read_csr(epc_csr));
			return;
		}
		case 0x302: { // MRET
			uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
			mstatus = (mstatus & MSTATUS_MPIE) ? (mstatus | MSTATUS_MIE) : (mstatus & ~MSTATUS_MIE);
			mstatus |= MSTATUS_MPIE; // MPIE reset to 1 on return, per spec
			PrivMode target = (PrivMode)((mstatus & MSTATUS_MPP) >> 11);
			// MPV says whether the trap came from a virtual mode. It is
			// only meaningful below M, since M-mode is never virtual --
			// returning to M always clears V.
			bool target_virt = Extensions.H && (mstatus & MSTATUS_MPV) != 0
			                && target != PrivMode::M;
			mstatus &= ~MSTATUS_MPP; // MPP reset to U on return, per spec
			mstatus &= ~MSTATUS_MPV;
			regs.write_csr(CSR_MSTATUS, mstatus);
			if (Extensions.H) regs.set_virt(target_virt);
			regs.set_priv(target);
			regs.set_pc(regs.read_csr(CSR_MEPC));
			return;
		}
		case 0x105: { // WFI
			// Treating the wait itself as a no-op is fine -- it is a hint,
			// never a mandatory wait, and check_and_take_interrupt runs
			// again before the next fetch regardless.
			//
			// Whether it is *allowed* is a different question. mstatus.TW
			// traps it from S-mode, and hstatus.VTW traps it from VS-mode,
			// so that a hypervisor can decide what a guest halting means
			// rather than having the guest silently continue.
			//
			// TW outranks VTW: when M-mode has closed WFI to everything
			// below it, the guest gets an illegal instruction (cause 2) and
			// the trap goes to M, not a virtual instruction handled by a
			// hypervisor that is itself denied the instruction.
			constexpr uint64_t MSTATUS_TW = 1ull << 21;
			if (regs.get_priv() != PrivMode::M
			    && (regs.read_csr(CSR_MSTATUS) & MSTATUS_TW)) {
				raise_illegal_instruction(regs, instr.raw);
				return;
			}
			if (Extensions.H && regs.get_virt() && regs.get_priv() == PrivMode::S
			    && (regs.read_csr(hyp::CSR_HSTATUS_ADDR) & (1ull << 21))) { // VTW
				enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
				return;
			}
			regs.set_pc(pc + instr.length);
			return;
		}
		default:
			// Genuinely unrecognized SYSTEM encoding.
			regs.set_pc(pc + instr.length);
			return;
		}
	}

	uint16_t csr = (uint16_t)instr.imm;

	// A guest reaching for the hypervisor's own registers is attempting
	// something only HS-mode may do, which is a *virtual* instruction
	// exception rather than an illegal one. The difference is what lets a
	// hypervisor emulate the access on the guest's behalf instead of
	// killing it, so it has to be decided before the ordinary privilege
	// check below turns it into cause 2.
	if (Extensions.H && hyp::is_virtual_instruction_csr(regs, csr)) {
		enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
		return;
	}

	// hstatus.VTVM does for a guest supervisor what mstatus.TVM does for a
	// real one: satp becomes unreachable, so the hypervisor sees every
	// attempt the guest makes to install or inspect its own root table.
	//
	// The cause is 22, not 2. Refusing with an illegal instruction tells
	// the guest it did something forbidden; a virtual instruction tells the
	// hypervisor the guest did something only the hypervisor may do, which
	// it can then emulate. This has to be checked *before* redirection,
	// while the number is still satp rather than vsatp.
	if (Extensions.H && regs.get_virt() && regs.get_priv() == PrivMode::S
	    && csr == CSR_SATP
	    && (regs.read_csr(hyp::CSR_HSTATUS_ADDR) & (1ull << 20))) { // VTVM
		enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
		return;
	}

	// In VS-mode the S-mode CSR names refer to the VS shadows. Rewriting
	// the number here, rather than special-casing each register at its own
	// read and write site, is what stops one of them being missed.
	if (Extensions.H) csr = hyp::redirect_for_virt(regs, csr);

	// Whether this instruction writes has to be decided before the access
	// check, not after: writing a read-only CSR is illegal, but *reading*
	// one is fine, and CSRRS/CSRRC with rs1==0 (the `csrr` pseudo-
	// instruction) is a read even though its encoding is a
	// read-modify-write. Deciding this later would make every csrr of a
	// read-only counter trap.
	bool writes = (instr.funct3 & 0x3) == 0b01 || instr.rs1 != 0;

	if (!csr_access_permitted(regs, csr, writes)) {
		// A guest refused by its hypervisor's state-enable gate gets a
		// virtual instruction, not an illegal one -- the hypervisor set
		// that gate and is entitled to be told when the guest hits it.
		// Only hstateen produces this; a refusal from mstateen is the
		// machine's, and stays cause 2.
		if (Extensions.SSSTATEEN && stateen::is_stateen_csr(csr)
		    && stateen::stateen_denial_is_virtual(regs, csr)) {
			enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
			return;
		}
		// tval is the whole instruction for an illegal-instruction trap,
		// which is what a handler needs to work out which CSR was refused.
		raise_illegal_instruction(regs, instr.raw);
		return;
	}

	regs.record_csr_access(csr); // dashboard's CSRs panel -- see registers.hpp
	uint64_t old = read_csr_effective(regs, mem, csr);

	// The *I forms (funct3 bit 2 set) use the rs1 field as a 5-bit
	// zero-extended immediate instead of a register number.
	uint64_t operand = (instr.funct3 & 0x4) ? instr.rs1 : regs.read_x(instr.rs1);

	uint64_t updated = old;
	// mtopei/stopei's claim-on-write side effect must NOT fire for a pure
	// read (CSRRS/CSRRC with rs1==0, e.g. the `csrr` pseudo-instruction --
	// unlike every other CSR here, "rewrite the same value" is not
	// harmless for these two, since the claim happens regardless of what
	// value is nominally written).
	bool did_write = writes;
	switch (instr.funct3 & 0x3) {
	case 0b01: updated = operand; break; // CSRRW/CSRRWI -- always writes
	case 0b10: if (instr.rs1 != 0) updated = old | operand; break;  // CSRRS/CSRRSI -- rs1/uimm==0 means read-only
	case 0b11: if (instr.rs1 != 0) updated = old & ~operand; break; // CSRRC/CSRRCI
	}

	// stvec/mtvec MODE (bits 1:0) is WARL, and this hart implements only
	// direct mode -- every trap goes to the base address regardless of
	// cause. Storing a mode it does not implement would let software read
	// back a vectored setting that is not honoured, so it is clamped on
	// write rather than merely ignored on use.
	//
	// vstvec is in this list because it *is* stvec as far as a guest is
	// concerned: the same field with the same rule, reached through the
	// same name. Keying the clamp on the S-mode number alone missed it,
	// since VS redirection has already rewritten the number by this point
	// -- the guest's write arrives here as 0x205, not 0x105.
	if (csr == CSR_STVEC || csr == CSR_MTVEC || csr == 0x205) updated &= ~0x3ull;

	// The PMM field of menvcfg/senvcfg/henvcfg (bits 33:32) selects the
	// pointer-masking length: 0 is off, 2 is PMLEN=7, 3 is PMLEN=16. Value
	// 1 is reserved, and WARL means a reserved value must never be readable
	// back -- software probes this field precisely by writing a value and
	// seeing what it gets, so storing 1 and privately treating it as "off"
	// tells the prober this hart implements a length it does not. Retaining
	// the previous legal field is the ordinary WARL response.
	if (Extensions.SSNPM && (csr == CSR_MENVCFG || csr == 0x10A
	                         || (Extensions.H && csr == 0x60A))) {
		constexpr uint64_t PMM = 3ull << 32;
		if (((updated >> 32) & 0x3) == 1) updated = (updated & ~PMM) | (old & PMM);
	}

	// CBIE is WARL in all three envcfg registers, and henvcfg additionally
	// cannot set a bit menvcfg has cleared -- see henvcfg_mask.
	if (csr == CSR_MENVCFG || csr == 0x10A || (Extensions.H && csr == 0x60A))
		updated = cbie_warl(updated, old);
	if (Extensions.H && csr == 0x60A)
		updated = (regs.read_csr(0x60A) & ~henvcfg_mask(regs))
		        | (updated & henvcfg_mask(regs));

	// hedeleg has read-only-zero bits, and they are not an arbitrary
	// restriction -- each one names a trap the hypervisor must keep.
	//
	//   10  ECALL from VS-mode. This *is* how a guest calls its
	//       hypervisor. Delegating it back to the guest would leave the
	//       guest unable to call out at all.
	//   20  instruction guest-page fault
	//   21  load guest-page fault
	//   23  store/AMO guest-page fault
	//       All three mean the second stage refused, which is the
	//       hypervisor's own mapping failing -- the guest cannot fix what
	//       it cannot see.
	//   22  virtual instruction. Raised precisely because the guest
	//       attempted something only the hypervisor may do; handing it to
	//       the guest would defeat the purpose.
	if (Extensions.H && csr == 0x602) {
		constexpr uint64_t HEDELEG_RO_ZERO =
			(1ull << 10) | (1ull << 20) | (1ull << 21) | (1ull << 22) | (1ull << 23);
		updated &= ~HEDELEG_RO_ZERO;
	}

	// hideleg can only delegate the three VS-level interrupts. The
	// hypervisor's own supervisor interrupts are not the guest's to take,
	// and there is no meaning to delegating an M-level one downward twice.
	if (Extensions.H && csr == 0x603) {
		constexpr uint64_t HIDELEG_WMASK =
			(1ull << 2) | (1ull << 6) | (1ull << 10); // VSSIP, VSTIP, VSEIP
		updated &= HIDELEG_WMASK;
	}

	if (pmp::is_pmpcfg(csr)) pmp::write_cfg(regs, csr, updated);
	else if (pmp::is_pmpaddr(csr)) pmp::write_addr(regs, csr, updated);
	else if (csr == 0x100) write_sstatus(regs, updated);
	else if (Extensions.H && csr == hyp::CSR_HSTATUS_ADDR) hyp::write_hstatus(regs, updated);
	else if (Extensions.SSCOFPMF && sscofpmf::is_mhpmevent(csr))
		regs.write_csr(csr, updated & sscofpmf::mhpmevent_wmask());
	else if (Extensions.SSSTATEEN && stateen::is_stateen_csr(csr)) stateen::write_stateen(regs, csr, updated);
	else if (csr == CSR_SATP) write_satp(regs, updated);
	// vsatp is satp as far as a guest is concerned -- same MODE field,
	// same WARL rule, reached through the same name. It needs the same
	// clamping for the same reason vstvec did above, and for the same
	// reason it is easy to miss: redirection has already rewritten the
	// number, so keying on CSR_SATP alone never sees the guest's write.
	//
	// Not covered by a test yet: vsatp's MODE only becomes observable once
	// two-stage translation reads it, which is the next increment.
	else if (Extensions.H && csr == 0x280) write_satp_warl(regs, 0x280, updated);
	else if (Extensions.H && csr == hyp::CSR_HGATP_ADDR) write_hgatp_warl(regs, updated);
	else if (csr == CSR_MIDELEG)
		regs.write_csr(CSR_MIDELEG, updated | mideleg_fixed_ones());
	else if (Extensions.H && (csr == CSR_HGEIE || csr == CSR_HGEIP)) { /* GEILEN=0 */ }
	else if (Extensions.H && csr == CSR_HIP) write_hip(regs, updated);
	else if (Extensions.H && csr == CSR_HIE) write_hie(regs, updated);
	else if (Extensions.H && csr == CSR_HVIP) write_hvip(regs, updated);
	else if (csr == CSR_SIE) write_sie(regs, updated);
	else if (csr == CSR_SIP) write_sip(regs, updated);
	else if (csr == CSR_MIP) regs.write_csr(CSR_MIP, updated & MIP_SHADOW_MASK);
	else if (csr == CSR_MIREG) mem.get_imsic_m().write_indirect(regs.read_csr(CSR_MISELECT), updated);
	else if (csr == CSR_SIREG) mem.get_imsic_s().write_indirect(regs.read_csr(CSR_SISELECT), updated);
	else if (csr == CSR_MTOPEI) { if (did_write) mem.get_imsic_m().claim(); }
	else if (csr == CSR_STOPEI) { if (did_write) mem.get_imsic_s().claim(); }
	else if (csr == CSR_MTOPI || csr == CSR_STOPI) { /* read-only */ }
	else if (csr == 0x001) regs.set_fflags((uint8_t)updated);
	else if (csr == 0x002) regs.set_frm((uint8_t)updated);
	else if (csr == 0x003) { regs.set_frm((uint8_t)(updated >> 5)); regs.set_fflags((uint8_t)updated); }
	else if (csr == 0x008) regs.set_vstart(updated);
	else if (csr == 0x009) regs.set_vxsat((uint8_t)updated);
	else if (csr == 0x00A) regs.set_vxrm((uint8_t)updated);
	else if (csr == 0x00F) { regs.set_vxrm((uint8_t)(updated >> 1)); regs.set_vxsat((uint8_t)updated); }
	else regs.write_csr(csr, updated);

	regs.write_x(instr.rd, old);
	regs.set_pc(pc + instr.length);
}
