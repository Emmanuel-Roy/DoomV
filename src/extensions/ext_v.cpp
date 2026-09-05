// V extension: top-level decode (field extraction + display mnemonic) and
// the exec_V dispatcher, which routes to the category handlers implemented
// across the other ext_v_*.cpp files (config, load/store, integer, mul/div,
// fixed-point, mask, permutation, reduction, floating-point). Mnemonic
// strings here are display-only (trace log) -- exec_V independently
// re-derives everything it needs from funct3/funct7/rs1/rs2 the same way
// every other extension's exec_* does, so a wrong/generic mnemonic can
// never cause an execution bug, only a less helpful trace line.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include "ext_v_common.hpp"

using namespace vcommon;

namespace {

// funct6 -> base mnemonic for the common OPIVV/OPIVX/OPIVI integer family
// (vadd, vsub, ...). Category suffix (.vv/.vx/.vi) is appended by the
// caller. Returns "???" for funct6 values this table doesn't name (mostly
// OPMVV/OPMVX-only or reduction/mask ops, named separately below).
const char *ivv_ivx_ivi_name(uint8_t funct6)
{
	switch (funct6) {
	case 0b000000: return "VADD";
	case 0b000010: return "VSUB";
	case 0b000011: return "VRSUB";
	case 0b000100: return "VMINU";
	case 0b000101: return "VMIN";
	case 0b000110: return "VMAXU";
	case 0b000111: return "VMAX";
	case 0b001001: return "VAND";
	case 0b001010: return "VOR";
	case 0b001011: return "VXOR";
	case 0b001100: return "VRGATHER";
	case 0b001110: return "VSLIDEUP";   // VSLIDE1UP under OPMVX shares this slot differently -- see funct3
	case 0b001111: return "VSLIDEDOWN";
	case 0b010111: return "VMV/VMERGE"; // vm=1,vs1=0 -> VMV.V.*; vm=0 -> VMERGE.VVM etc
	case 0b011000: return "VMSEQ";
	case 0b011001: return "VMSNE";
	case 0b011010: return "VMSLTU";
	case 0b011011: return "VMSLT";
	case 0b011100: return "VMSLEU";
	case 0b011101: return "VMSLE";
	case 0b011110: return "VMSGTU";
	case 0b011111: return "VMSGT";
	case 0b100000: return "VSADDU";
	case 0b100001: return "VSADD";
	case 0b100010: return "VSSUBU";
	case 0b100011: return "VSSUB";
	case 0b100101: return "VSLL";
	case 0b100111: return "VSMUL/VMVNR";
	case 0b101000: return "VSRL";
	case 0b101001: return "VSRA";
	case 0b101010: return "VSSRL";
	case 0b101011: return "VSSRA";
	case 0b101100: return "VNSRL";
	case 0b101101: return "VNSRA";
	case 0b101110: return "VNCLIPU";
	case 0b101111: return "VNCLIP";
	default: return "???";
	}
}

// funct6 -> base mnemonic for OPMVV/OPMVX (multiply/divide, widening,
// averaging, reductions, mask-scalar moves, mask logical ops).
const char *mvv_mvx_name(uint8_t funct6, bool is_mvv)
{
	switch (funct6) {
	case 0b000000: return "VREDSUM/VAADDU";
	case 0b000001: return "VREDAND/VAADD";
	case 0b000010: return "VREDOR/VASUBU";
	case 0b000011: return "VREDXOR/VASUB";
	case 0b000100: return "VREDMINU";
	case 0b000101: return "VREDMIN";
	case 0b000110: return "VREDMAXU";
	case 0b000111: return "VREDMAX";
	case 0b001000: return "VAADDU";
	case 0b001001: return "VAADD";
	case 0b001010: return "VASUBU";
	case 0b001011: return "VASUB";
	case 0b001100: return is_mvv ? "VMV.X.S/VCOMPRESS" : "VSLIDE1UP";
	case 0b001111: return "VSLIDE1DOWN";
	case 0b010000: return is_mvv ? "VMV.X.S/VCPOP" : "VMV.S.X";
	case 0b010001: return "VMANDNOT/VFIRST";
	case 0b010010: return "VMAND/VMSBF";
	case 0b010011: return "VMOR/VMSIF";
	case 0b010100: return "VMXOR/VMSOF";
	case 0b010101: return "VMORNOT/VIOTA";
	case 0b010110: return "VMNAND/VID";
	case 0b010111: return "VMNOR";
	case 0b011000: return "VMXNOR";
	case 0b100000: return "VDIVU";
	case 0b100001: return "VDIV";
	case 0b100010: return "VREMU";
	case 0b100011: return "VREM";
	case 0b100100: return "VMULHU";
	case 0b100101: return "VMUL";
	case 0b100110: return "VMULHSU";
	case 0b100111: return "VMULH";
	case 0b101001: return "VMADD";
	case 0b101011: return "VNMSUB";
	case 0b101101: return "VMACC";
	case 0b101111: return "VNMSAC";
	case 0b110000: return "VWADDU";
	case 0b110001: return "VWADD";
	case 0b110010: return "VWSUBU";
	case 0b110011: return "VWSUB";
	case 0b110100: return "VWADDU.W";
	case 0b110101: return "VWADD.W";
	case 0b110110: return "VWSUBU.W";
	case 0b110111: return "VWSUB.W";
	case 0b111000: return "VWMULU";
	case 0b111010: return "VWMULSU";
	case 0b111011: return "VWMUL";
	case 0b111100: return "VWMACCU";
	case 0b111101: return "VWMACC";
	case 0b111110: return is_mvv ? "???" : "VWMACCUS";
	case 0b111111: return "VWMACCSU";
	default: return "???";
	}
}

const char *fvv_fvf_name(uint8_t funct6)
{
	switch (funct6) {
	case 0b000000: return "VFADD";
	case 0b000010: return "VFSUB";
	case 0b100111: return "VFRSUB"; // .vf only
	case 0b100100: return "VFDIV";
	case 0b100101: return "VFRDIV"; // .vf only
	case 0b100000: return "VFMUL";
	case 0b100011: return "VFRSQRT7/VFSQRT/VFCLASS";
	case 0b001000: return "VFSGNJ";
	case 0b001001: return "VFSGNJN";
	case 0b001010: return "VFSGNJX";
	case 0b000100: return "VFMIN";
	case 0b000110: return "VFMAX";
	case 0b011000: return "VMFEQ";
	case 0b011100: return "VMFLE";
	case 0b011011: return "VMFLT";
	case 0b011101: return "VMFNE";
	case 0b011111: return "VMFGT";
	case 0b011110: return "VMFGE";
	case 0b100010: return "VFMV/VFMERGE";
	case 0b101000: return "VFMADD";
	case 0b101001: return "VFNMADD";
	case 0b101010: return "VFMSUB";
	case 0b101011: return "VFNMSUB";
	case 0b101100: return "VFMACC";
	case 0b101101: return "VFNMACC";
	case 0b101110: return "VFMSAC";
	case 0b101111: return "VFNMSAC";
	case 0b110000: return "VFWADD";
	case 0b110010: return "VFWSUB";
	case 0b110100: return "VFWADD.W";
	case 0b110110: return "VFWSUB.W";
	case 0b111000: return "VFWMUL";
	case 0b111100: return "VFWMACC";
	case 0b111101: return "VFWNMACC";
	case 0b111110: return "VFWMSAC";
	case 0b111111: return "VFWNMSAC";
	case 0b010000: return "VFCVT";
	case 0b010010: return "VFWCVT";
	case 0b010011: return "VFNCVT";
	case 0b000001: return "VFREDUSUM";
	case 0b000011: return "VFREDOSUM";
	case 0b000101: return "VFREDMIN";
	case 0b000111: return "VFREDMAX";
	case 0b110001: return "VFWREDUSUM";
	case 0b110011: return "VFWREDOSUM";
	default: return "???";
	}
}

} // namespace

DecodedInstruction Decoder::decode_v(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::V;
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

	if (opcode == 0b0000111 || opcode == 0b0100111) { // vector load/store
		bool is_load = (opcode == 0b0000111);
		uint8_t mop = ldst_mop(funct7);
		static const char *widths[8] = {"8", "???", "???", "???", "???", "16", "32", "64"};
		static const char *modes[4] = {"US", "S", "UXEI", "OXEI"};
		instr.mnemonic = is_load ? "VLE/VLS/VLX" : "VSE/VSS/VSX";
		(void)mop; (void)widths; (void)modes; // exact name resolved textually below for the common cases
		char buf_unused;
		(void)buf_unused;
		if (mop == 0b00) instr.mnemonic = is_load ? "VLE.V" : "VSE.V";
		else if (mop == 0b10) instr.mnemonic = is_load ? "VLSE.V" : "VSSE.V";
		else instr.mnemonic = is_load ? "VLXEI.V" : "VSXEI.V";
		return instr;
	}

	if (opcode != 0b1010111) return instr; // shouldn't happen -- decode() only routes these three opcodes here

	if (funct3 == 0b111) { // OPCFG: vsetvli / vsetvl / vsetivli
		bool bit31 = (funct7 >> 6) & 1;
		bool bit30 = (funct7 >> 5) & 1;
		if (!bit31) instr.mnemonic = "VSETVLI";
		else if (funct7 == 0b1000000) instr.mnemonic = "VSETVL";
		else if (bit30) instr.mnemonic = "VSETIVLI";
		return instr;
	}

	uint8_t funct6 = op_v_funct6(funct7);
	switch (funct3) {
	case 0b000: instr.mnemonic = ivv_ivx_ivi_name(funct6); break; // OPIVV
	case 0b100: instr.mnemonic = ivv_ivx_ivi_name(funct6); break; // OPIVX
	case 0b011: instr.mnemonic = ivv_ivx_ivi_name(funct6); break; // OPIVI
	case 0b010: instr.mnemonic = mvv_mvx_name(funct6, true);  break; // OPMVV
	case 0b110: instr.mnemonic = mvv_mvx_name(funct6, false); break; // OPMVX
	case 0b001: instr.mnemonic = fvv_fvf_name(funct6); break; // OPFVV
	case 0b101: instr.mnemonic = fvv_fvf_name(funct6); break; // OPFVF
	}

	return instr;
}

// Routes to the category handler that actually owns a given funct6 within
// OPIVV/OPIVX/OPIVI/OPMVV/OPMVX's shared encoding space -- cross-cutting
// dispatch logic (same role classify() plays for the base decoder), kept
// here rather than duplicated into every category file. Table derived from
// the authoritative riscv-opcodes rv_v encoding list, not recalled from
// memory: https://github.com/riscv/riscv-opcodes/blob/master/extensions/rv_v
void RiscvCore::exec_V(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	// Reaching here means the decoder already checked mstatus.VS is not
	// Off, so the unit is on and this instruction is about to touch vector
	// state. Mark it Dirty so a supervisor knows there is something to save.
	mark_vector_dirty(regs);

	if (instr.opcode == 0b0000111 || instr.opcode == 0b0100111) {
		// A page fault mid-instruction already redirected pc into the trap
		// handler -- must not then stomp it with the normal advance below.
		if (exec_v_ldst(instr, regs, mem, *this)) regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.funct3 == 0b111) { // OPCFG: vsetvli/vsetvl/vsetivli
		exec_v_config(instr, regs);
		regs.set_pc(pc + instr.length);
		return;
	}

	uint8_t funct6 = op_v_funct6(instr.funct7);
	bool vm = op_v_vm(instr.funct7);

	switch (instr.funct3) {
	case 0b000: case 0b100: case 0b011: { // OPIVV / OPIVX / OPIVI
		bool is_ivi = (instr.funct3 == 0b011);
		if (funct6 == 0x0c || funct6 == 0x0e || funct6 == 0x0f) exec_v_perm(instr, regs); // rgather*/slideup/slidedown
		else if (is_ivi && funct6 == 0x27 && vm) exec_v_perm(instr, regs);                // vmv<n>r.v
		else if (instr.funct3 == 0b000 && (funct6 == 0x30 || funct6 == 0x31)) exec_v_reduce(instr, regs); // vwredsum(u).vs
		else exec_v_int(instr, regs);
		break;
	}
	case 0b010: case 0b110: { // OPMVV / OPMVX
		bool is_mvv = (instr.funct3 == 0b010);
		if (is_mvv && funct6 <= 0x07) exec_v_reduce(instr, regs);
		else if (funct6 >= 0x08 && funct6 <= 0x0b) exec_v_muldiv(instr, regs); // averaging add/sub
		else if (!is_mvv && (funct6 == 0x0e || funct6 == 0x0f)) exec_v_perm(instr, regs); // vslide1up/down.vx
		else if (funct6 == 0x10) exec_v_mask(instr, regs); // vmv.x.s/vcpop.m/vfirst.m or vmv.s.x
		else if (is_mvv && funct6 == 0x12) exec_v_mask(instr, regs); // vext (vzext/vsext)
		else if (is_mvv && funct6 == 0x14) exec_v_mask(instr, regs); // vmsbf/vmsof/vmsif/viota/vid
		else if (is_mvv && funct6 == 0x17) exec_v_perm(instr, regs); // vcompress.vm
		else if (is_mvv && funct6 >= 0x18 && funct6 <= 0x1f) exec_v_mask(instr, regs); // mask logical (mm)
		else exec_v_muldiv(instr, regs); // div/rem/mulh/madd/macc + widening add/sub/mul/macc
		break;
	}
	case 0b001: case 0b101: // OPFVV / OPFVF
		exec_v_fp(instr, regs);
		break;
	}

	regs.set_pc(pc + instr.length);
}
