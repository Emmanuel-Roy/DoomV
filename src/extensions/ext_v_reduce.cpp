// Integer reductions: vred{sum,and,or,xor,minu,min,maxu,max}.vs (OPMVV) and
// the two widening reductions vwredsumu.vs/vwredsum.vs (OPIVV). All reduce
// vs2's active elements into a single result, seeded by vs1's element 0
// (the *initial* accumulator value, not an elementwise operand -- vs1 is
// otherwise unused), written to vd's element 0 only; every other vd element
// is left undisturbed.
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>

using namespace vcommon;

namespace vcommon {

void exec_v_reduce(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	uint8_t funct6 = op_v_funct6(instr.funct7);
	bool vm = op_v_vm(instr.funct7);
	uint64_t smask = elem_mask(sew);

	if (instr.funct3 == 0b000) { // OPIVV: vwredsumu.vs (0x30, unsigned) / vwredsum.vs (0x31, signed)
		int wide = sew * 2;
		if (wide > 64) return; // SEW=64 has no <=64-bit-representable wide result -- reserved, no-op
		bool is_signed = (funct6 == 0x31);
		uint64_t wmask = elem_mask(wide);
		__int128 acc = is_signed ? (__int128)sext_elem(read_velem(regs, instr.rs1, wide, 0), wide)
		                         : (__int128)read_velem(regs, instr.rs1, wide, 0);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			__int128 v = is_signed ? (__int128)sext_elem(read_velem(regs, instr.rs2, sew, i), sew)
			                       : (__int128)read_velem(regs, instr.rs2, sew, i);
			acc += v;
		});
		if (vl > 0) write_velem(regs, instr.rd, wide, 0, (uint64_t)acc & wmask);
		return;
	}

	// OPMVV: single-width reductions.
	uint64_t acc_u = read_velem(regs, instr.rs1, sew, 0);
	int64_t acc_s = sext_elem(acc_u, sew);
	for_each_active(regs, vm, vl, [&](uint64_t i) {
		uint64_t vu = read_velem(regs, instr.rs2, sew, i);
		int64_t vs = sext_elem(vu, sew);
		switch (funct6) {
		case 0x00: acc_u = (acc_u + vu) & smask; break; // vredsum
		case 0x01: acc_u &= vu; break;                  // vredand
		case 0x02: acc_u |= vu; break;                  // vredor
		case 0x03: acc_u ^= vu; break;                  // vredxor
		case 0x04: if (vu < acc_u) acc_u = vu; break;   // vredminu
		case 0x05: if (vs < acc_s) acc_s = vs; break;   // vredmin
		case 0x06: if (vu > acc_u) acc_u = vu; break;   // vredmaxu
		case 0x07: if (vs > acc_s) acc_s = vs; break;   // vredmax
		}
	});

	bool signed_result = (funct6 == 0x05 || funct6 == 0x07);
	if (vl > 0) write_velem(regs, instr.rd, sew, 0, signed_result ? ((uint64_t)acc_s & smask) : acc_u);
}

} // namespace vcommon
