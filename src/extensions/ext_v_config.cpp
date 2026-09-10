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

// ELEN, as a power of two: the widest element this vector unit supports.
// 64 here, matching the Sail configuration this machine is diffed against
// (extensions.V.elen_exp = 6) and RVA23's Zve64d.
static constexpr int ELEN_POW = 6;

void exec_v_config(const DecodedInstruction &instr, Registers &regs)
{
	uint8_t f7 = instr.funct7;
	bool bit31 = (f7 >> 6) & 1;

	uint64_t raw_vtype;
	uint64_t avl = 0;
	bool vlmax_request = false;
	bool keep_vl = false; // rd=x0,rs1=x0 form: vtype changes, vl is left exactly as it was
	// Bits above the four defined fields are reserved, and a nonzero one
	// makes the whole vtype illegal rather than being ignored. Where they
	// live differs per form: bits[10:8] of vsetvli's 11-bit immediate,
	// bits[9:8] of vsetivli's 10-bit one, and bits[62:8] plus vill itself of
	// the register vsetvl reads. That is the point of the field -- it is how
	// software discovers that this hart does not implement whatever a future
	// extension puts there, and ignoring it would report success for a
	// configuration the machine is not providing.
	bool reserved_set = false;

	if (!bit31) { // vsetvli rd, rs1, zimm[10:0] -- zimm spans bits[30:20] = (funct7&0x3F)<<5 | rs2
		raw_vtype = ((uint64_t)(f7 & 0x3F) << 5) | instr.rs2;
		reserved_set = (raw_vtype >> 8) != 0;
		if (instr.rs1 == 0 && instr.rd == 0) keep_vl = true;
		else if (instr.rs1 == 0) vlmax_request = true;
		else avl = regs.read_x(instr.rs1);
	} else if (f7 == 0b1000000) { // vsetvl rd, rs1, rs2 -- vtype comes from the rs2 *register's value*, not an immediate
		raw_vtype = regs.read_x(instr.rs2);
		reserved_set = (raw_vtype >> 8) != 0; // covers the reserved field and vill
		if (instr.rs1 == 0 && instr.rd == 0) keep_vl = true;
		else if (instr.rs1 == 0) vlmax_request = true;
		else avl = regs.read_x(instr.rs1);
	} else { // vsetivli rd, uimm5, zimm[9:0] -- zimm spans bits[29:20]; AVL is instr.rs1's raw 5-bit
		// field value itself (the vs1/rs1 position holds a literal uimm5 here, not a register number).
		raw_vtype = ((uint64_t)(f7 & 0x1F) << 5) | instr.rs2;
		reserved_set = (raw_vtype >> 8) != 0;
		avl = instr.rs1;
	}

	VType vt = decode_vtype(raw_vtype);
	if (reserved_set) vt.vill = true;

	// SEW may not exceed ELEN, and for a fractional LMUL it may not exceed
	// LMUL * ELEN -- "implementations must support SEW settings between
	// SEWMIN and LMUL * ELEN, inclusive", which says nothing about above it.
	// At ELEN=64 that makes every fractional LMUL illegal at SEW=64, LMUL of
	// 1/4 and 1/8 illegal at SEW=32, and 1/8 illegal at SEW=16.
	//
	// Without this a whole family of encodings was accepted and given a
	// VLMAX computed by integer division, which for LMUL=1/8 at SEW=64
	// truncates to zero: an accepted configuration in which no vector
	// instruction can address an element.
	{
		int sew_pow = 3;
		for (int w = vt.sew; w > 8; w >>= 1) sew_pow++;
		const int lmul_pow = (vt.lmul_den == 1)
		                   ? __builtin_ctz((unsigned)vt.lmul_num)
		                   : -__builtin_ctz((unsigned)vt.lmul_den);
		if (sew_pow > ELEN_POW || sew_pow > lmul_pow + ELEN_POW) vt.vill = true;

		// The rd=x0, rs1=x0 form keeps vl, which is only meaningful if VLMAX
		// is unchanged -- so it is illegal to use it to change the SEW/LMUL
		// ratio, or to use it at all while vtype is already vill.
		if (keep_vl && !vt.vill) {
			const VType cur = decode_vtype(regs.get_vtype());
			int cur_sew_pow = 3;
			for (int w = cur.sew; w > 8; w >>= 1) cur_sew_pow++;
			const int cur_lmul_pow = (cur.lmul_den == 1)
			                       ? __builtin_ctz((unsigned)cur.lmul_num)
			                       : -__builtin_ctz((unsigned)cur.lmul_den);
			if (cur.vill || (cur_lmul_pow - cur_sew_pow) != (lmul_pow - sew_pow))
				vt.vill = true;
		}
	}

	uint64_t vlmax_val = vlmax(vt);

	uint64_t new_vl;
	if (keep_vl) new_vl = regs.get_vl();
	else if (vlmax_request) new_vl = vlmax_val;
	else new_vl = (avl < vlmax_val) ? avl : vlmax_val; // vl = min(AVL, VLMAX)

	if (vt.vill) {
		new_vl = 0;
		regs.set_vtype(1ull << 63);
	} else {
		// Only the four defined fields are stored. Writing the request back
		// verbatim would let a reserved bit be read out of vtype again, and
		// the reserved check above has already established they are zero --
		// masking says so in the code rather than relying on it.
		regs.set_vtype(raw_vtype & 0xFF);
	}

	regs.set_vl(new_vl);
	regs.set_vstart(0); // any vset{i}vl{i} resets vstart, even the vtype-only rd=x0/rs1=x0 form
	regs.write_x(instr.rd, new_vl);
}

} // namespace vcommon
