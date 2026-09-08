// H: hypervisor load/store and the guest fences.
//
// The second increment of the hypervisor extension. hlv/hsv let HS-mode
// reach guest memory *as the guest would* -- through the guest's own page
// tables, at the guest's own privilege, faulting where the guest would
// fault. That is the entire reason they exist: a hypervisor emulating a
// guest instruction has to read the guest's operands, and simply
// dereferencing the address would use the hypervisor's mappings and
// permissions instead, which is both wrong and a security hole.
//
// Encodings, from the assembler. All SYSTEM with funct3=100 for the
// accesses; funct7 is even for a load and odd for a store, stepping in
// pairs by width:
//
//   0x30/0x31  byte     hlv.b  (rs2=0) hlv.bu (rs2=1)  / hsv.b
//   0x32/0x33  half     hlv.h  (rs2=0) hlv.hu (rs2=1)  / hsv.h
//              plus     hlvx.hu (rs2=3)
//   0x34/0x35  word     hlv.w  (rs2=0) hlv.wu (rs2=1)  / hsv.w
//              plus     hlvx.wu (rs2=3)
//   0x36/0x37  double   hlv.d                          / hsv.d
//
// Two collisions worth stating, because both would silently mis-decode:
//
//   * funct3=100 is also Zimop's MOP.R space. They are separated by bit 31,
//     which MOP.R sets and hlv/hsv leave clear.
//
//   * hfence.gvma's funct7 (0x31) is the same as hsv.b's. They are
//     separated by funct3 -- the fences use 000, the accesses 100.
//
// The fences themselves have nothing to invalidate here, for the same
// reason sfence.vma and Svinval do not: mmu_translate walks the tables in
// guest memory on every access, so no translation can be stale.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "mmu.hpp"
#include "extensions.hpp"
#include "ext_h.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_h_ldst(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::H;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	if (instr.funct3 == 0) { // hfence.vvma / hfence.gvma
		instr.mnemonic = (instr.funct7 == 0x11) ? "HFENCE.VVMA" : "HFENCE.GVMA";
		return instr;
	}

	switch (instr.funct7) {
	case 0x30: instr.mnemonic = (instr.rs2 == 1) ? "HLV.BU" : "HLV.B"; break;
	case 0x31: instr.mnemonic = "HSV.B"; break;
	case 0x32: instr.mnemonic = (instr.rs2 == 3) ? "HLVX.HU" : (instr.rs2 == 1 ? "HLV.HU" : "HLV.H"); break;
	case 0x33: instr.mnemonic = "HSV.H"; break;
	case 0x34: instr.mnemonic = (instr.rs2 == 3) ? "HLVX.WU" : (instr.rs2 == 1 ? "HLV.WU" : "HLV.W"); break;
	case 0x35: instr.mnemonic = "HSV.W"; break;
	case 0x36: instr.mnemonic = "HLV.D"; break;
	case 0x37: instr.mnemonic = "HSV.D"; break;
	}
	return instr;
}

void RiscvCore::exec_H(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	// The fences have nothing to invalidate on a machine that re-walks the
	// page tables for every access.
	if (instr.funct3 == 0) {
		// The fences themselves are no-ops here -- there is no TLB to
		// invalidate -- but *who may issue them* is still observable, and
		// this returned before the privilege check below ever ran. A guest
		// supervisor could execute HFENCE.VVMA and HFENCE.GVMA, which are
		// the hypervisor's instructions for managing the guest's own
		// translation, and have them quietly succeed.
		//
		// From a virtual mode it is a virtual instruction, not an illegal
		// one, so the hypervisor can emulate the invalidation.
		if (regs.get_virt()) {
			enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
			return;
		}
		if (regs.get_priv() == PrivMode::U) {
			raise_illegal_instruction(regs, instr.raw);
			return;
		}
		regs.set_pc(regs.get_pc() + instr.length);
		return;
	}

	// These are hypervisor instructions. HS-mode and M may issue them; a
	// guest may not, and U-mode only when the hypervisor has opted in via
	// hstatus.HU. Getting this wrong would let a guest read arbitrary
	// memory through its own page tables at supervisor permission.
	PrivMode priv = regs.get_priv();
	if (regs.get_virt()) {
		// From VS or VU this is a virtual instruction, not an illegal
		// one: the hypervisor can emulate it on the guest's behalf.
		enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
		return;
	}
	if (priv == PrivMode::U && !(regs.read_csr(0x600) & (1ull << 9))) { // hstatus.HU
		raise_illegal_instruction(regs, instr.raw);
		return;
	}

	bool is_store = (instr.funct7 & 1) != 0;
	int width = 1 << ((instr.funct7 - 0x30) >> 1); // 0x30/1 -> 1, 0x32/3 -> 2, ...
	uint64_t addr = regs.read_x(instr.rs1);

	uint64_t paddr, cause, tval;
	// hlvx reads with *execute* permission rather than read permission --
	// it exists so a hypervisor can fetch a guest instruction it is about
	// to emulate from a page the guest may execute but not read. Asking
	// for AccessType::Fetch is what expresses that; using Load would
	// wrongly succeed on a read-only page and wrongly fail on an
	// execute-only one.
	bool is_hlvx = !is_store && instr.rs2 == 3;
	AccessType type = is_store ? AccessType::Store
	                : is_hlvx ? AccessType::Fetch
	                          : AccessType::Load;

	if (!mmu_translate(regs, mem, addr, type, paddr, cause, tval, /*as_guest=*/true)) {
		enter_trap(regs, cause, tval);
		return;
	}

	if (is_store) {
		uint64_t val = regs.read_x(instr.rs2);
		switch (width) {
		case 1: mem.write8(paddr, (uint8_t)val); break;
		case 2: mem.write8(paddr, (uint8_t)val); mem.write8(paddr + 1, (uint8_t)(val >> 8)); break;
		case 4: mem.write32(paddr, (uint32_t)val); break;
		default: mem.write64(paddr, val); break;
		}
	} else {
		// rs2 selects signedness for the narrow forms: 0 is the signed
		// load, 1 the unsigned one. hlvx (rs2 == 3) is always unsigned.
		bool is_unsigned = (instr.rs2 == 1 || instr.rs2 == 3);
		uint64_t val = 0;
		switch (width) {
		case 1:
			val = mem.read8(paddr);
			if (!is_unsigned) val = (uint64_t)(int64_t)(int8_t)val;
			break;
		case 2:
			val = (uint64_t)(mem.read8(paddr) | ((uint16_t)mem.read8(paddr + 1) << 8));
			if (!is_unsigned) val = (uint64_t)(int64_t)(int16_t)val;
			break;
		case 4:
			val = mem.read32(paddr);
			if (!is_unsigned) val = (uint64_t)(int64_t)(int32_t)val;
			break;
		default:
			val = mem.read64(paddr);
			break;
		}
		regs.write_x(instr.rd, val);
	}

	regs.set_pc(regs.get_pc() + instr.length);
}
