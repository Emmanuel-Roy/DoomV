// Vector loads/stores: unit-stride (incl. whole-register, mask, and
// fault-only-first forms), strided, and indexed (ordered/unordered, treated
// identically since this emulator is single-threaded so element ordering
// never observably matters), all with segment (nf) support.
//
// Fault-only-first is still just treated like a normal unit-stride load --
// implementing its actual point (let a load past the first element
// terminate early and shrink vl instead of trapping) would need knowing a
// fault is *about to* happen without taking it, which the current
// translate-and-trap-immediately plumbing doesn't distinguish. Since a
// spurious page fault where FOF would have quietly shrunk vl is strictly
// more correct than this emulator's old "no MMU exists" behavior, this is
// a known gap, not a regression.
//
// Precise mid-instruction fault behavior (per spec, vstart should land
// exactly on the faulting element for a possible restart) isn't
// implemented either -- a fault sets a flag checked before each remaining
// element's access so nothing after the fault touches memory, but vl/
// vstart aren't trimmed. V is opt-in and not required for anything this
// project boots yet, so this is deferred rather than solved preemptively.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "ext_v_common.hpp"

using namespace vcommon;

namespace {

uint64_t ld_eew(Memory &mem, uint64_t addr, int eew)
{
	switch (eew) {
	case 8:  return mem.read8(addr);
	case 16: return mem.read16(addr);
	case 32: return mem.read32(addr);
	default: return mem.read64(addr);
	}
}

// Memory has no write16 (nothing before this needed one -- see ext_i.cpp's
// SH handling for the same two-write8 split).
void st_eew(Memory &mem, uint64_t addr, int eew, uint64_t value)
{
	switch (eew) {
	case 8:
		mem.write8(addr, (uint8_t)value);
		break;
	case 16:
		mem.write8(addr, (uint8_t)(value & 0xFF));
		mem.write8(addr + 1, (uint8_t)((value >> 8) & 0xFF));
		break;
	case 32:
		mem.write32(addr, (uint32_t)value);
		break;
	default:
		mem.write64(addr, value);
		break;
	}
}

} // namespace

namespace vcommon {

bool exec_v_ldst(const DecodedInstruction &instr, Registers &regs, Memory &mem, RiscvCore &core)
{
	bool is_load = (instr.opcode == 0b0000111);
	AccessType access = is_load ? AccessType::Load : AccessType::Store;
	uint8_t f7 = instr.funct7;
	uint8_t nf = ldst_nf(f7) + 1; // segment count (1 = no segmentation)
	uint8_t mop = ldst_mop(f7);
	bool vm = ldst_vm(f7);

	static const int width_tab[8] = {8, 0, 0, 0, 0, 16, 32, 64};
	int inst_eew = width_tab[instr.funct3];

	uint64_t base_addr = regs.read_x(instr.rs1);
	uint8_t lumop = instr.rs2; // only meaningful when mop==0b00

	// Whole-register load/store (vlNre<eew>.v / vsNr.v): mop=0, lumop=0x08.
	// The nf field is repurposed here to hold N-1 (register count minus
	// one) -- same bit position (31:29), different meaning for this one
	// sub-family. Ignores vl/vstart/masking entirely, per spec.
	if (mop == 0b00 && lumop == 0x08) {
		int nreg = nf;
		uint64_t total = (uint64_t)nreg * Registers::VLEN_BITS / inst_eew;
		for (uint64_t i = 0; i < total; i++) {
			uint64_t addr = base_addr + i * (uint64_t)(inst_eew / 8);
			uint64_t paddr;
			if (!core.translate_or_trap(regs, mem, addr, access, paddr)) return false;
			if (is_load) write_velem(regs, instr.rd, inst_eew, i, ld_eew(mem, paddr, inst_eew));
			else st_eew(mem, paddr, inst_eew, read_velem(regs, instr.rd, inst_eew, i));
		}
		return true;
	}

	// Mask load/store (vlm.v/vsm.v): mop=0, lumop=0x0b. Always EEW=8,
	// ceil(vl/8) bytes, unconditionally unmasked, ignores current SEW/LMUL.
	if (mop == 0b00 && lumop == 0x0b) {
		uint64_t vl = regs.get_vl();
		uint64_t nbytes = (vl + 7) / 8;
		for (uint64_t i = 0; i < nbytes; i++) {
			uint64_t addr = base_addr + i;
			uint64_t paddr;
			if (!core.translate_or_trap(regs, mem, addr, access, paddr)) return false;
			if (is_load) write_velem(regs, instr.rd, 8, i, ld_eew(mem, paddr, 8));
			else st_eew(mem, paddr, 8, read_velem(regs, instr.rd, 8, i));
		}
		return true;
	}

	// Everything else -- normal/fault-only-first unit-stride, strided,
	// indexed -- shares the vl/vstart/mask-driven element loop.
	uint64_t vl = regs.get_vl();
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	bool indexed = (mop == 0b01 || mop == 0b11);
	int data_eew = indexed ? sew : inst_eew;   // indexed: data is at the vtype's own SEW
	int idx_eew = inst_eew;                     // indexed: the instruction's width field describes the *index*

	// EMUL for the data operand, ratio-preserving against the vtype's LMUL
	// (EEW/SEW * LMUL) -- for indexed accesses data_eew==sew so this is
	// just LMUL unchanged, which is the spec-correct behavior there too.
	int emul_num = vt.lmul_num * data_eew;
	int emul_den = vt.lmul_den * sew;
	int span = (emul_num > emul_den) ? (emul_num / emul_den) : 1; // physical registers per segment field

	// See the file header: a fault mid-instruction just stops any further
	// memory access for the remaining elements rather than precisely
	// trimming vl/vstart. `faulted` short-circuits every remaining call of
	// the lambda below rather than actually breaking the loop, since
	// for_each_active always runs it to completion.
	bool faulted = false;
	for_each_active(regs, vm, vl, [&](uint64_t i) {
		if (faulted) return;
		uint64_t seg_addr;
		if (mop == 0b10) { // strided: rs2 is a signed byte stride
			int64_t stride = (int64_t)regs.read_x(instr.rs2);
			seg_addr = base_addr + (uint64_t)((int64_t)i * stride);
		} else if (indexed) { // rs2 (vs2) is an index vector, EEW = inst_eew, raw byte offset
			uint64_t idx_val = read_velem(regs, instr.rs2, idx_eew, i);
			seg_addr = base_addr + idx_val;
		} else { // unit-stride: segments packed tightly, nf*eew bytes apart
			seg_addr = base_addr + i * (uint64_t)nf * (uint64_t)(data_eew / 8);
		}

		for (int f = 0; f < nf; f++) {
			uint64_t field_addr = seg_addr + (uint64_t)f * (uint64_t)(data_eew / 8);
			int reg_base = instr.rd + f * span;
			uint64_t paddr;
			if (!core.translate_or_trap(regs, mem, field_addr, access, paddr)) { faulted = true; return; }
			if (is_load) write_velem(regs, reg_base, data_eew, i, ld_eew(mem, paddr, data_eew));
			else st_eew(mem, paddr, data_eew, read_velem(regs, reg_base, data_eew, i));
		}
	});
	return !faulted;
}

} // namespace vcommon
