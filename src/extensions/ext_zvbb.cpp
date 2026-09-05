// Zvbb: vector basic bit manipulation. Only vandn.vv/vandn.vx is
// implemented, because that is the one a distro riscv64 glibc actually
// emits alongside the base vector ops -- the rest of Zvbb (vbrev, vclz,
// vctz, vcpop, vrol, vror, vwsll) has no consumer here yet, and guessing at
// implementations nothing exercises is how the mask-register bug got in.
//
// It occupies OPIVV/OPIVX funct6 0x01, a slot base V leaves reserved, so it
// does not collide with anything. Not gated separately from V: a userspace
// that has vandn.vv in its string routines has it unconditionally, so a
// separate toggle would only add another way to configure a guest into
// failing.
//
// The operand plumbing is re-derived here rather than shared with
// exec_v_int -- same convention the rest of the vector files follow, each
// deriving vtype/vl/SEW and its own operand selection.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"

using namespace vcommon;

namespace vcommon {

void exec_zvbb(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	bool vm = op_v_vm(instr.funct7);
	bool is_vv = (instr.funct3 == 0b000);
	uint64_t smask = elem_mask(sew);

	// vandn takes its second operand from vs1 (.vv) or an x-register
	// truncated to SEW (.vx). There is no .vi form.
	auto op2 = [&](uint64_t i) -> uint64_t {
		if (is_vv) return read_velem(regs, instr.rs1, sew, i);
		return regs.read_x(instr.rs1) & smask;
	};

	// vd = vs2 & ~op2
	for_each_active(regs, vm, vl, [&](uint64_t i) {
		write_velem(regs, instr.rd, sew, i, (read_velem(regs, instr.rs2, sew, i) & ~op2(i)) & smask);
	});
}

} // namespace vcommon
