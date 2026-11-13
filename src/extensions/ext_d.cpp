// D: double-precision floating point -- its decode, and its loads and
// stores. Everything else runs in the shared floating-point unit (fpu.cpp),
// which is SoftFloat on bit patterns; see fpu.hpp.
//
// D shares every opcode with F, split in Decoder::classify(): OP-FP on
// funct7's low bit (odd is double), LOAD-FP/STORE-FP on funct3, and the
// fused multiply-add family on funct2.
//
// The two conversions between the widths live here rather than in ext_f.cpp
// because they involve both formats and the spec lists them under D --
// FCVT.S.D narrows and FCVT.D.S widens. Both require F *and* D to be
// enabled, unlike every other instruction in either file, which is enforced
// in decode_d() below.
//
// The f registers are 64 bits wide, so double values need no boxing.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "ext_xstate.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include "fpu.hpp"

DecodedInstruction Decoder::decode_d(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::D;
	instr.length = 4;
	instr.mnemonic = "???";
	instr.fp_double = true;

	uint8_t opcode = raw_instr & 0x7F;
	uint8_t funct3 = (raw_instr >> 12) & 0x07;
	uint8_t rs2    = (raw_instr >> 20) & 0x1F;
	uint8_t funct7 = (raw_instr >> 25) & 0x7F;

	instr.opcode = opcode;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = rs2;
	instr.funct3 = funct3;
	instr.funct7 = funct7;

	int32_t imm_i = (int32_t)raw_instr >> 20;
	int32_t imm_s = (((raw_instr >> 25) & 0x7F) << 5) | ((raw_instr >> 7) & 0x1F);
	if (raw_instr & 0x80000000) imm_s |= 0xFFFFF000;

	switch (opcode) {
	case 0b0000111: // LOAD-FP
		instr.imm = imm_i;
		instr.mnemonic = "FLD";
		break;
	case 0b0100111: // STORE-FP
		instr.imm = imm_s;
		instr.mnemonic = "FSD";
		break;

	// Fused multiply-add: rs3 is the fourth operand, in funct7's high bits.
	case 0b1000011: instr.rs3 = funct7 >> 2; instr.mnemonic = "FMADD.D";  break;
	case 0b1000111: instr.rs3 = funct7 >> 2; instr.mnemonic = "FMSUB.D";  break;
	case 0b1001011: instr.rs3 = funct7 >> 2; instr.mnemonic = "FNMSUB.D"; break;
	case 0b1001111: instr.rs3 = funct7 >> 2; instr.mnemonic = "FNMADD.D"; break;

	case 0b1010011: // OP-FP
		switch (funct7) {
		case 0b0000001: instr.mnemonic = "FADD.D";  break;
		case 0b0000101: instr.mnemonic = "FSUB.D";  break;
		case 0b0001001: instr.mnemonic = "FMUL.D";  break;
		case 0b0001101: instr.mnemonic = "FDIV.D";  break;
		case 0b0101101: instr.mnemonic = "FSQRT.D"; break;
		case 0b0010001:
			switch (funct3) {
			case 0b000: instr.mnemonic = "FSGNJ.D";  break;
			case 0b001: instr.mnemonic = "FSGNJN.D"; break;
			case 0b010: instr.mnemonic = "FSGNJX.D"; break;
			}
			break;
		case 0b0010101: instr.mnemonic = (funct3 == 0b001) ? "FMAX.D" : "FMIN.D"; break;
		case 0b1010001: // FLE/FLT/FEQ.D -- rd is an integer register
			switch (funct3) {
			case 0b000: instr.mnemonic = "FLE.D"; break;
			case 0b001: instr.mnemonic = "FLT.D"; break;
			case 0b010: instr.mnemonic = "FEQ.D"; break;
			}
			break;
		case 0b1100001: // FCVT.W/WU/L/LU.D -- rd integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.W.D";  break;
			case 0b00001: instr.mnemonic = "FCVT.WU.D"; break;
			case 0b00010: instr.mnemonic = "FCVT.L.D";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.LU.D"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1101001: // FCVT.D.W/WU/L/LU -- rs1 integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.D.W";  break;
			case 0b00001: instr.mnemonic = "FCVT.D.WU"; break;
			case 0b00010: instr.mnemonic = "FCVT.D.L";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.D.LU"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1110001: // rs1 D -> rd integer: FMV.X.D (RV64 only) / FCLASS.D
			if (funct3 == 0b001) {
				instr.mnemonic = "FCLASS.D";
			} else {
				instr.mnemonic = "FMV.X.D";
				if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL;
			}
			break;
		case 0b1111001: // FMV.D.X (RV64 only)
			instr.mnemonic = "FMV.D.X";
			if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL;
			break;

		// The two width-crossing conversions. Unlike everything else in
		// either file these need *both* flags enabled, not just one.
		case 0b0100000:
			instr.mnemonic = "FCVT.S.D";
			if (!Extensions.F || !Extensions.D) instr.ext = Extension::ILLEGAL;
			break;
		case 0b0100001:
			instr.mnemonic = "FCVT.D.S";
			if (!Extensions.F || !Extensions.D) instr.ext = Extension::ILLEGAL;
			break;
		}
		break;

	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_D(const DecodedOp &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	// The decoder already established mstatus.FS is not Off. Whether this
	// instruction makes it Dirty depends on what it writes; see
	// FpDirtyWhenWritten.
	vcommon::FpDirtyWhenWritten fp_dirty(regs);

	if (instr.opcode == 0b0000111) { // FLD -- rs1 is an integer base register
		// As FLW does it -- see exec_F.
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t v;
		if (!load_virtual(regs, mem, addr, 8, v)) return;
		regs.write_f_bits(instr.rd, v);
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b0100111) { // FSD -- raw bits
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;   // as FLD
		if (!store_virtual(regs, mem, addr, 8, regs.read_f_bits(instr.rs2))) return;
		regs.set_pc(pc + instr.length);
		return;
	}

	// Everything else is the shared unit, at double precision -- including
	// FCVT.S.D and FCVT.D.S, whose format field names the destination.
	fpu::execute(instr, regs);
	regs.set_pc(pc + instr.length);
}
