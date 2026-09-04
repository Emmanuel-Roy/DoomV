// Zicsr extension: ECALL/EBREAK/MRET control-transfer plus CSR read/modify/
// write. Also owns the M-mode trap-entry sequence (enter_trap), since ECALL/
// EBREAK are the only things in this project that ever trigger one.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include "timer.hpp"
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
		else if (funct7 == 0b0001001 && rd == 0) instr.mnemonic = "SFENCE.VMA";
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
// mtopei/stopei do. mtopi/stopi (read-only priority summaries) aren't
// implemented -- nothing in this stage's verification needs them, and
// leaving them at their generic-array default (0) is spec-plausible
// enough not to be worth the extra surface right now.
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
constexpr uint64_t MIP_SHADOW_MASK = MIP_SSIP | MIP_MSIP | MIP_SEIP | MIP_STIP;

constexpr uint64_t MENVCFG_STCE = 1ull << 63;

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

// sstatus is architecturally just the bits of mstatus a lower-privileged
// mode is allowed to see/touch -- SUM/MXR are read-write pass-through,
// SIE/SPIE/SPP alias the same-named mstatus bits directly.
constexpr uint64_t SSTATUS_MASK = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | (1ull << 18) | (1ull << 19);

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
	bit('S');
	bit('U');
	v |= (Extensions.XLEN64 ? 2ull : 1ull) << (Extensions.XLEN64 ? 62 : 30);
	return v;
}

// satp.MODE is WARL (Write Any, Read Legal): real hardware that doesn't
// implement a given paging mode clamps an unsupported MODE write so a
// readback never reports support that isn't really there. mmu.cpp only
// implements MODE 0 (bare) and 8 (Sv39) -- everything else used to just
// fall through to plain csr[] storage, meaning a write of an unsupported
// mode read back exactly as written.
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
void write_satp(Registers &regs, uint64_t value)
{
	uint64_t mode = value >> 60;
	if (mode != 0 && mode != 8) return; // reject the whole write, not just the MODE field -- matches real WARL clamping
	regs.write_csr(CSR_SATP, value);
}

uint64_t read_sstatus(Registers &regs)
{
	return regs.read_csr(CSR_MSTATUS) & SSTATUS_MASK;
}

void write_sstatus(Registers &regs, uint64_t value)
{
	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
	mstatus = (mstatus & ~SSTATUS_MASK) | (value & SSTATUS_MASK);
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
uint64_t RiscvCore::read_csr_effective(Registers &regs, Memory &mem, uint16_t csr)
{
	if (csr == 0x100) return read_sstatus(regs);
	if (csr == CSR_MISA) return compute_misa();
	if (csr == CSR_SIE) return read_sie(regs);
	if (csr == CSR_SIP) return read_sip(regs, mem);
	if (csr == CSR_MIP) return compute_mip(regs, mem);
	if (csr == CSR_MIREG) return mem.get_imsic_m().read_indirect(regs.read_csr(CSR_MISELECT));
	if (csr == CSR_SIREG) return mem.get_imsic_s().read_indirect(regs.read_csr(CSR_SISELECT));
	if (csr == CSR_MTOPEI) return mem.get_imsic_m().topei_value();
	if (csr == CSR_STOPEI) return mem.get_imsic_s().topei_value();
	if (csr == CSR_MTOPI) return compute_topi(regs, mem, /*s_level=*/false);
	if (csr == CSR_STOPI) return compute_topi(regs, mem, /*s_level=*/true);
	if (csr == CSR_TIME) return mem.get_timer().get_mtime();
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

bool RiscvCore::translate_or_trap(Registers &regs, Memory &mem, uint64_t vaddr, AccessType type, uint64_t &paddr)
{
	uint64_t cause, tval;
	if (mmu_translate(regs, mem, vaddr, type, paddr, cause, tval)) return true;
	enter_trap(regs, cause, tval);
	return false;
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

	if (to_s) {
		regs.write_csr(CSR_SEPC, pc);
		regs.write_csr(CSR_SCAUSE, cause);
		regs.write_csr(CSR_STVAL, tval);

		uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
		mstatus = (mstatus & MSTATUS_SIE) ? (mstatus | MSTATUS_SPIE) : (mstatus & ~MSTATUS_SPIE);
		mstatus &= ~MSTATUS_SIE;
		mstatus = (from == PrivMode::S) ? (mstatus | MSTATUS_SPP) : (mstatus & ~MSTATUS_SPP);
		regs.write_csr(CSR_MSTATUS, mstatus);

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
	regs.write_csr(CSR_MSTATUS, mstatus);

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

	// Fixed priority order per spec: MEI > MSI > MTI > SEI > SSI > STI.
	static const int priority_order[] = {
		(int)CAUSE_M_EXTERNAL, (int)CAUSE_M_SOFTWARE, (int)CAUSE_M_TIMER,
		(int)CAUSE_S_EXTERNAL, (int)CAUSE_S_SOFTWARE, (int)CAUSE_S_TIMER,
	};
	int bit = -1;
	for (int b : priority_order) {
		if (pending_enabled & (1ull << b)) { bit = b; break; }
	}
	if (bit < 0) return false; // only a bit outside this fixed set is pending -- nothing this project defines yet

	PrivMode priv = regs.get_priv();
	bool to_s = (regs.read_csr(CSR_MIDELEG) & (1ull << bit)) != 0;
	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);

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
			enter_trap(regs, cause, 0);
			return;
		}
		case 0x001: // EBREAK
			enter_trap(regs, CAUSE_BREAKPOINT, pc);
			return;
		case 0x102: { // SRET -- mirrors MRET below, using the S-mode fields
			uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
			mstatus = (mstatus & MSTATUS_SPIE) ? (mstatus | MSTATUS_SIE) : (mstatus & ~MSTATUS_SIE);
			mstatus |= MSTATUS_SPIE; // SPIE reset to 1 on return, per spec
			PrivMode target = (mstatus & MSTATUS_SPP) ? PrivMode::S : PrivMode::U;
			mstatus &= ~MSTATUS_SPP; // SPP reset to U on return, per spec
			regs.write_csr(CSR_MSTATUS, mstatus);
			regs.set_priv(target);
			regs.set_pc(regs.read_csr(CSR_SEPC));
			return;
		}
		case 0x302: { // MRET
			uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
			mstatus = (mstatus & MSTATUS_MPIE) ? (mstatus | MSTATUS_MIE) : (mstatus & ~MSTATUS_MIE);
			mstatus |= MSTATUS_MPIE; // MPIE reset to 1 on return, per spec
			PrivMode target = (PrivMode)((mstatus & MSTATUS_MPP) >> 11);
			mstatus &= ~MSTATUS_MPP; // MPP reset to U on return, per spec
			regs.write_csr(CSR_MSTATUS, mstatus);
			regs.set_priv(target);
			regs.set_pc(regs.read_csr(CSR_MEPC));
			return;
		}
		case 0x105: // WFI -- always legal to treat as a plain, immediate
			// no-op (it's only ever a hint, never a mandatory wait), and
			// simplest here: check_and_take_interrupt runs again before
			// the very next fetch regardless, so nothing is lost by not
			// actually blocking until mip & mie is nonzero.
			regs.set_pc(pc + instr.length);
			return;
		default:
			// Genuinely unrecognized SYSTEM encoding.
			regs.set_pc(pc + instr.length);
			return;
		}
	}

	uint16_t csr = (uint16_t)instr.imm;
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
	bool did_write = true;
	switch (instr.funct3 & 0x3) {
	case 0b01: updated = operand; break; // CSRRW/CSRRWI -- always writes
	case 0b10: if (instr.rs1 != 0) updated = old | operand; else did_write = false; break;  // CSRRS/CSRRSI -- rs1/uimm==0 means read-only
	case 0b11: if (instr.rs1 != 0) updated = old & ~operand; else did_write = false; break; // CSRRC/CSRRCI
	}

	if (csr == 0x100) write_sstatus(regs, updated);
	else if (csr == CSR_SATP) write_satp(regs, updated);
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
