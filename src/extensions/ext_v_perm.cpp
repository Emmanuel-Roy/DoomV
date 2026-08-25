// Permutation instructions: register gather (vrgather{,ei16}), compress
// (vcompress.vm), slide up/down (regular and by-one), and whole-register
// move (vmv<n>r.v). Spread across OPIVV/OPIVX/OPIVI/OPMVV/OPMVX depending
// on the specific op -- exec_V's dispatcher already sorted out which
// funct6/funct3 combination means what before calling in here.
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>
#include <cstring>

using namespace vcommon;

namespace {
bool reg_bit(const Registers &regs, int reg, uint64_t idx) { return (regs.read_v(reg)[idx / 8] >> (idx % 8)) & 1; }
}

namespace vcommon {

void exec_v_perm(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	uint8_t funct6 = op_v_funct6(instr.funct7);
	bool vm = op_v_vm(instr.funct7);

	// vmv<n>r.v -- OPIVI, funct6=0x27, bit25(vm) fixed 1. Raw whole-register
	// copy, N = (rs1 field value)+1 registers; ignores vl/masking entirely.
	if (instr.funct3 == 0b011 && funct6 == 0x27) {
		int n = instr.rs1 + 1;
		for (int r = 0; r < n; r++) {
			std::memcpy(regs.write_v(instr.rd + r), regs.read_v(instr.rs2 + r), Registers::VLEN_BYTES);
		}
		return;
	}

	if (instr.funct3 == 0b010 && funct6 == 0x17) { // vcompress.vm (OPMVV) -- vs1 is a select mask, not v0
		uint64_t out = 0;
		for_each(regs, vl, [&](uint64_t i) {
			if (reg_bit(regs, instr.rs1, i)) {
				write_velem(regs, instr.rd, sew, out, read_velem(regs, instr.rs2, sew, i));
				out++;
			}
		});
		return;
	}

	if (instr.funct3 == 0b110 && (funct6 == 0x0e || funct6 == 0x0f)) { // vslide1up.vx / vslide1down.vx (OPMVX)
		uint64_t smask = elem_mask(sew);
		uint64_t scalar = regs.read_x(instr.rs1) & smask;
		if (funct6 == 0x0e) { // vslide1up: vd[0]=scalar, vd[i]=vs2[i-1] for i>=1
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				uint64_t r = (i == 0) ? scalar : read_velem(regs, instr.rs2, sew, i - 1);
				write_velem(regs, instr.rd, sew, i, r);
			});
		} else { // vslide1down: vd[i]=vs2[i+1] for i<vl-1, vd[vl-1]=scalar
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				uint64_t r = (i + 1 < vl) ? read_velem(regs, instr.rs2, sew, i + 1) : scalar;
				write_velem(regs, instr.rd, sew, i, r);
			});
		}
		return;
	}

	if (funct6 == 0x0c) { // vrgather.vv (OPIVV) / vrgather.vx (OPIVX) / vrgather.vi (OPIVI)
		uint64_t vlmax_val = vlmax(vt);
		bool is_vv = (instr.funct3 == 0b000);
		bool is_vx = (instr.funct3 == 0b100);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t idx = is_vv ? read_velem(regs, instr.rs1, sew, i)
			             : is_vx ? regs.read_x(instr.rs1)
			             : (uint64_t)instr.rs1; // vi: raw unsigned 0-31
			uint64_t r = (idx < vlmax_val) ? read_velem(regs, instr.rs2, sew, idx) : 0;
			write_velem(regs, instr.rd, sew, i, r);
		});
		return;
	}

	if (instr.funct3 == 0b000 && funct6 == 0x0e) { // vrgatherei16.vv -- index vector always EEW=16
		uint64_t vlmax_val = vlmax(vt);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t idx = read_velem(regs, instr.rs1, 16, i);
			uint64_t r = (idx < vlmax_val) ? read_velem(regs, instr.rs2, sew, idx) : 0;
			write_velem(regs, instr.rd, sew, i, r);
		});
		return;
	}

	if ((instr.funct3 == 0b100 || instr.funct3 == 0b011) && (funct6 == 0x0e || funct6 == 0x0f)) {
		// vslideup.vx/.vi (0x0e) / vslidedown.vx/.vi (0x0f)
		bool is_vx = (instr.funct3 == 0b100);
		uint64_t offset = is_vx ? regs.read_x(instr.rs1) : (uint64_t)instr.rs1; // vi: raw unsigned 0-31
		uint64_t vlmax_val = vlmax(vt);
		if (funct6 == 0x0e) { // slideup: elements below `offset` left undisturbed
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				if (i < offset) return;
				write_velem(regs, instr.rd, sew, i, read_velem(regs, instr.rs2, sew, i - offset));
			});
		} else { // slidedown: source indices at/past VLMAX read as 0
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				uint64_t src = i + offset;
				uint64_t r = (src < vlmax_val) ? read_velem(regs, instr.rs2, sew, src) : 0;
				write_velem(regs, instr.rd, sew, i, r);
			});
		}
		return;
	}
}

} // namespace vcommon
