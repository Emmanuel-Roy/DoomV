// The OPMVV/OPMVX "mask and unary" family: mask-register logical ops
// (vmand.mm etc), the funct6=0x10 scalar<->mask-element moves (vmv.x.s/
// vmv.s.x) plus mask population-count/find-first (vcpop.m/vfirst.m), the
// integer-extend family (vzext/vsext.vfN), and the mask prefix-scan family
// (vmsbf/vmsof/vmsif.m/viota.m/vid.v).
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>

using namespace vcommon;

namespace {

// Like vcommon::mask_bit, but for an arbitrary source register instead of
// always v0 -- mask-logical ops' vs1/vs2 are ordinary register numbers
// holding mask data, not necessarily v0.
bool reg_bit(const Registers &regs, int reg, uint64_t idx)
{
	return (regs.read_v(reg)[idx / 8] >> (idx % 8)) & 1;
}

} // namespace

namespace vcommon {

void exec_v_mask(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	uint8_t funct6 = op_v_funct6(instr.funct7);
	bool vm = op_v_vm(instr.funct7);
	bool is_mvv = (instr.funct3 == 0b010);

	// Mask-register logical ops (vmand.mm etc): bit-for-bit over [0,vl),
	// unconditional (no separate masking concept applies to these).
	if (funct6 >= 0x18 && funct6 <= 0x1f) {
		for_each(regs, vl, [&](uint64_t i) {
			bool a = reg_bit(regs, instr.rs2, i), b = reg_bit(regs, instr.rs1, i);
			bool r;
			switch (funct6) {
			case 0x18: r = a && !b; break;       // vmandn
			case 0x19: r = a && b; break;         // vmand
			case 0x1a: r = a || b; break;         // vmor
			case 0x1b: r = a != b; break;         // vmxor
			case 0x1c: r = a || !b; break;        // vmorn
			case 0x1d: r = !(a && b); break;      // vmnand
			case 0x1e: r = !(a || b); break;      // vmnor
			default:   r = !(a != b); break;      // vmxnor (0x1f)
			}
			set_mask_bit(regs, instr.rd, i, r);
		});
		return;
	}

	if (funct6 == 0x10) {
		if (!is_mvv) { // vmv.s.x: vd[0] = low SEW bits of rs1; every other element left undisturbed
			if (vl > 0) write_velem(regs, instr.rd, sew, 0, regs.read_x(instr.rs1) & elem_mask(sew));
			return;
		}
		uint8_t sub = instr.rs1; // vs1 field selects the sub-op
		if (sub == 0x00) { // vmv.x.s: rd = sign-extended vs2[0] (vl/masking irrelevant -- always reads element 0)
			regs.write_x(instr.rd, (uint64_t)sext_elem(read_velem(regs, instr.rs2, sew, 0), sew));
			return;
		}
		if (sub == 0x10) { // vcpop.m: population count of vs2's mask bits over active elements
			uint64_t count = 0;
			for_each_active(regs, vm, vl, [&](uint64_t i) { if (reg_bit(regs, instr.rs2, i)) count++; });
			regs.write_x(instr.rd, count);
			return;
		}
		if (sub == 0x11) { // vfirst.m: index of the first active element with vs2's mask bit set, or -1
			int64_t first = -1;
			uint64_t vstart = regs.get_vstart();
			for (uint64_t i = vstart; i < vl && first < 0; i++) {
				if (elem_active(regs, vm, i) && reg_bit(regs, instr.rs2, i)) first = (int64_t)i;
			}
			regs.set_vstart(0);
			regs.write_x(instr.rd, (uint64_t)first);
			return;
		}
		return;
	}

	if (is_mvv && funct6 == 0x12) { // vzext.vfN / vsext.vfN -- vs1 field selects divisor and signedness
		uint8_t sub = instr.rs1;
		int divisor = 0;
		bool is_signed = false;
		switch (sub) {
		case 2: divisor = 8; is_signed = false; break;
		case 3: divisor = 8; is_signed = true;  break;
		case 4: divisor = 4; is_signed = false; break;
		case 5: divisor = 4; is_signed = true;  break;
		case 6: divisor = 2; is_signed = false; break;
		case 7: divisor = 2; is_signed = true;  break;
		default: return;
		}
		int src_eew = sew / divisor;
		if (src_eew < 8) return; // reserved for this SEW/divisor combination -- no-op
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t raw = read_velem(regs, instr.rs2, src_eew, i);
			uint64_t r = is_signed ? ((uint64_t)sext_elem(raw, src_eew) & elem_mask(sew)) : raw;
			write_velem(regs, instr.rd, sew, i, r);
		});
		return;
	}

	if (is_mvv && funct6 == 0x14) { // vmsbf.m / vmsof.m / vmsif.m / viota.m / vid.v -- vs1 field selects
		uint8_t sub = instr.rs1;
		if (sub == 0x01 || sub == 0x02 || sub == 0x03) { // vmsbf/vmsof/vmsif: sequential prefix scan over active elements
			bool found = false;
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				bool bit = reg_bit(regs, instr.rs2, i);
				bool out;
				if (sub == 0x01) { out = (!found && !bit); if (!found && bit) found = true; }        // vmsbf
				else if (sub == 0x02) { out = (!found && bit); if (out) found = true; }               // vmsof
				else { out = !found; if (!found && bit) found = true; }                                // vmsif
				set_mask_bit(regs, instr.rd, i, out);
			});
			return;
		}
		if (sub == 0x10) { // viota.m: exclusive running population count of vs2 over active elements, written as data
			uint64_t count = 0;
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, sew, i, count);
				if (reg_bit(regs, instr.rs2, i)) count++;
			});
			return;
		}
		if (sub == 0x11) { // vid.v: vd[i] = i
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, sew, i, i & elem_mask(sew));
			});
			return;
		}
	}
}

} // namespace vcommon
