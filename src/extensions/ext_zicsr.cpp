// Zicsr extension: ECALL/EBREAK/MRET control-transfer plus CSR read/modify/
// write. Also owns the M-mode trap-entry sequence (enter_trap), since ECALL/
// EBREAK are the only things in this project that ever trigger one.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"

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
		// else: unimplemented privileged op (WFI, SFENCE.VMA, ...) --
		// mnemonic stays "???"; exec_32ZICSR treats it as a no-op since
		// nothing in this project runs an OS that would emit one.
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
// Anything else (mscratch, misa, mhartid, ...) is still fully readable/
// writable -- Registers::csr[] backs all 4096 addresses generically -- it
// just has no side effects, which is correct for those.
constexpr uint16_t CSR_MSTATUS = 0x300;
constexpr uint16_t CSR_MTVEC   = 0x305;
constexpr uint16_t CSR_MEPC    = 0x341;
constexpr uint16_t CSR_MCAUSE  = 0x342;
constexpr uint16_t CSR_MTVAL   = 0x343;

constexpr uint64_t MSTATUS_MIE  = 1ull << 3;
constexpr uint64_t MSTATUS_MPIE = 1ull << 7;

constexpr uint64_t CAUSE_BREAKPOINT   = 3;
constexpr uint64_t CAUSE_ECALL_FROM_M = 11;
}

void RiscvCore::enter_trap(Registers &regs, uint64_t cause, uint64_t tval)
{
	uint64_t pc = regs.get_pc();
	regs.write_csr(CSR_MEPC, pc);
	regs.write_csr(CSR_MCAUSE, cause);
	regs.write_csr(CSR_MTVAL, tval);

	// Standard M-mode enable stacking: the current interrupt-enable bit is
	// saved to MPIE and cleared, so a handler doesn't get pre-empted by
	// itself; MRET reverses this.
	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
	mstatus = (mstatus & MSTATUS_MIE) ? (mstatus | MSTATUS_MPIE) : (mstatus & ~MSTATUS_MPIE);
	mstatus &= ~MSTATUS_MIE;
	regs.write_csr(CSR_MSTATUS, mstatus);

	// Direct mode only (mtvec[1:0] ignored) -- vectored mode's cause-based
	// offset only applies to interrupts, and this project has no interrupt
	// sources (no timer/external IRQ controller), only synchronous
	// exceptions, which always go to the base address regardless of mode.
	regs.set_pc(regs.read_csr(CSR_MTVEC) & ~0x3ull);
}

void RiscvCore::exec_32ZICSR(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();

	if (instr.funct3 == 0) {
		// ECALL/EBREAK/MRET -- control transfer, not a CSR read/modify/write.
		// Illegal-instruction detection deliberately stays a separate,
		// unconditional debugger halt (see DoomSystem::step) rather than a
		// real trap here: nothing in this project sets up mtvec to actually
		// handle one, so routing illegal instructions through this same
		// path would just spin forever re-trapping instead of surfacing a
		// crash log.
		switch (instr.imm) {
		case 0x000: // ECALL
			enter_trap(regs, CAUSE_ECALL_FROM_M, 0);
			return;
		case 0x001: // EBREAK
			enter_trap(regs, CAUSE_BREAKPOINT, pc);
			return;
		case 0x302: { // MRET
			uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
			mstatus = (mstatus & MSTATUS_MPIE) ? (mstatus | MSTATUS_MIE) : (mstatus & ~MSTATUS_MIE);
			mstatus |= MSTATUS_MPIE; // MPIE reset to 1 on return, per spec
			regs.write_csr(CSR_MSTATUS, mstatus);
			regs.set_pc(regs.read_csr(CSR_MEPC));
			return;
		}
		default:
			// Unimplemented privileged op (WFI, SFENCE.VMA, ...) -- not
			// expected without an OS; treated as a no-op like decode()'s
			// other unrecognized-but-enabled encodings.
			regs.set_pc(pc + instr.length);
			return;
		}
	}

	uint16_t csr = (uint16_t)instr.imm;
	// fflags(0x001)/frm(0x002)/fcsr(0x003) live in Registers' dedicated
	// fields, not the generic csr[] array -- fflags needs OR-accumulate
	// semantics from FP ops that a plain array slot can't express, and
	// fcsr is just those two fields packed together (frm in bits[7:5],
	// fflags in bits[4:0]).
	uint64_t old;
	if (csr == 0x001) old = regs.get_fflags();
	else if (csr == 0x002) old = regs.get_frm();
	else if (csr == 0x003) old = ((uint64_t)regs.get_frm() << 5) | regs.get_fflags();
	// V's own dedicated-field CSRs, same reasoning as fflags/frm/fcsr above.
	// vstart/vxsat/vxrm/vcsr are ordinary read-write; vl/vtype/vlenb are
	// read-only in practice (only vset{i}vl{i} ever changes them) -- a
	// write instruction targeting one still runs (real hardware would trap
	// as illegal, but nothing here relies on that), it just lands in the
	// generic csr[] array below and is never looked at again.
	else if (csr == 0x008) old = regs.get_vstart();
	else if (csr == 0x009) old = regs.get_vxsat();
	else if (csr == 0x00A) old = regs.get_vxrm();
	else if (csr == 0x00F) old = ((uint64_t)regs.get_vxrm() << 1) | regs.get_vxsat();
	else if (csr == 0xC20) old = regs.get_vl();
	else if (csr == 0xC21) old = regs.get_vtype();
	else if (csr == 0xC22) old = Registers::VLEN_BYTES;
	else old = regs.read_csr(csr);

	// The *I forms (funct3 bit 2 set) use the rs1 field as a 5-bit
	// zero-extended immediate instead of a register number.
	uint64_t operand = (instr.funct3 & 0x4) ? instr.rs1 : regs.read_x(instr.rs1);

	uint64_t updated = old;
	switch (instr.funct3 & 0x3) {
	case 0b01: updated = operand; break; // CSRRW/CSRRWI -- always writes
	case 0b10: if (instr.rs1 != 0) updated = old | operand; break;  // CSRRS/CSRRSI -- rs1/uimm==0 means read-only
	case 0b11: if (instr.rs1 != 0) updated = old & ~operand; break; // CSRRC/CSRRCI
	}

	if (csr == 0x001) regs.set_fflags((uint8_t)updated);
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
