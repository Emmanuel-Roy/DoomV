// V extension configuration: vsetvli / vsetvl / vsetivli. These three share
// OP-V's opcode and funct3=111 (OPCFG) but have entirely different bitfield
// layouts from each other and from every other V instruction, distinguished
// by bit 31 (and, when set, bit 30) of the raw word -- both already folded
// into instr.funct7 (=bits[31:25]) the same way every decode_* extracts it,
// so no new DecodedInstruction field was needed to tell them apart.
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"

using namespace vcommon;

namespace vcommon {

void exec_v_config(const DecodedInstruction &instr, Registers &regs)
{
	uint8_t f7 = instr.funct7;
	bool bit31 = (f7 >> 6) & 1;

	uint64_t raw_vtype;
	uint64_t avl = 0;
	bool vlmax_request = false;
	bool keep_vl = false; // rd=x0,rs1=x0 form: vtype changes, vl is left exactly as it was

	if (!bit31) { // vsetvli rd, rs1, zimm[10:0] -- zimm spans bits[30:20] = (funct7&0x3F)<<5 | rs2
		raw_vtype = ((uint64_t)(f7 & 0x3F) << 5) | instr.rs2;
		if (instr.rs1 == 0 && instr.rd == 0) keep_vl = true;
		else if (instr.rs1 == 0) vlmax_request = true;
		else avl = regs.read_x(instr.rs1);
	} else if (f7 == 0b1000000) { // vsetvl rd, rs1, rs2 -- vtype comes from the rs2 *register's value*, not an immediate
		raw_vtype = regs.read_x(instr.rs2);
		if (instr.rs1 == 0 && instr.rd == 0) keep_vl = true;
		else if (instr.rs1 == 0) vlmax_request = true;
		else avl = regs.read_x(instr.rs1);
	} else { // vsetivli rd, uimm5, zimm[9:0] -- zimm spans bits[29:20]; AVL is instr.rs1's raw 5-bit
		// field value itself (the vs1/rs1 position holds a literal uimm5 here, not a register number).
		raw_vtype = ((uint64_t)(f7 & 0x1F) << 5) | instr.rs2;
		avl = instr.rs1;
	}

	VType vt = decode_vtype(raw_vtype);
	uint64_t vlmax_val = vlmax(vt);

	uint64_t new_vl;
	if (keep_vl) new_vl = regs.get_vl();
	else if (vlmax_request) new_vl = vlmax_val;
	else new_vl = (avl < vlmax_val) ? avl : vlmax_val; // vl = min(AVL, VLMAX)

	if (vt.vill) {
		new_vl = 0;
		regs.set_vtype(1ull << 63);
	} else {
		regs.set_vtype(raw_vtype);
	}

	regs.set_vl(new_vl);
	regs.set_vstart(0); // any vset{i}vl{i} resets vstart, even the vtype-only rd=x0/rs1=x0 form
	regs.write_x(instr.rd, new_vl);
}

} // namespace vcommon
