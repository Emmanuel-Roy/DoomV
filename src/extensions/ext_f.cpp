// F: single-precision floating point. Uses the host's real IEEE-754
// hardware via <cfenv> rather than a from-scratch soft-float library --
// see ext_fp_common.hpp for the shared rounding-mode, exception-flag and
// NaN-canonicalisation plumbing that both F and D sit on.
//
// F and D share every one of their opcodes. OP-FP splits on funct7's low
// bit (even is single, odd is double), LOAD-FP/STORE-FP on funct3, and the
// fused multiply-add family on funct2 -- all decided in
// Decoder::classify(), so this file only ever sees the single-precision
// side and never has to branch on width.
//
// The two conversions between the widths, FCVT.S.D and FCVT.D.S, live in
// ext_d.cpp: they involve both formats, and the spec lists them under D.
//
// Single-precision values live NaN-boxed in the 64-bit f registers, so
// every read goes through read_f32_reg / write_f32_reg rather than touching
// the register directly.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "ext_xstate.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include "ext_fp_common.hpp"
#include <cfenv>
#include <climits>
#include <cmath>
#include <cstring>

DecodedInstruction Decoder::decode_f(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::F;
	instr.length = 4;
	instr.mnemonic = "???";
	instr.fp_double = false;

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
		instr.mnemonic = "FLW";
		break;
	case 0b0100111: // STORE-FP
		instr.imm = imm_s;
		instr.mnemonic = "FSW";
		break;

	// Fused multiply-add: rs3 is the fourth operand, in funct7's high bits.
	case 0b1000011: instr.rs3 = funct7 >> 2; instr.mnemonic = "FMADD.S";  break;
	case 0b1000111: instr.rs3 = funct7 >> 2; instr.mnemonic = "FMSUB.S";  break;
	case 0b1001011: instr.rs3 = funct7 >> 2; instr.mnemonic = "FNMSUB.S"; break;
	case 0b1001111: instr.rs3 = funct7 >> 2; instr.mnemonic = "FNMADD.S"; break;

	case 0b1010011: // OP-FP -- funct7 selects the operation, rs2 the conversion type
		switch (funct7) {
		case 0b0000000: instr.mnemonic = "FADD.S";  break;
		case 0b0000100: instr.mnemonic = "FSUB.S";  break;
		case 0b0001000: instr.mnemonic = "FMUL.S";  break;
		case 0b0001100: instr.mnemonic = "FDIV.S";  break;
		case 0b0101100: instr.mnemonic = "FSQRT.S"; break;
		case 0b0010000:
			switch (funct3) {
			case 0b000: instr.mnemonic = "FSGNJ.S";  break;
			case 0b001: instr.mnemonic = "FSGNJN.S"; break;
			case 0b010: instr.mnemonic = "FSGNJX.S"; break;
			}
			break;
		case 0b0010100: instr.mnemonic = (funct3 == 0b001) ? "FMAX.S" : "FMIN.S"; break;
		case 0b1010000: // FLE/FLT/FEQ.S -- rd is an integer register
			switch (funct3) {
			case 0b000: instr.mnemonic = "FLE.S"; break;
			case 0b001: instr.mnemonic = "FLT.S"; break;
			case 0b010: instr.mnemonic = "FEQ.S"; break;
			}
			break;
		case 0b1100000: // FCVT.W/WU/L/LU.S -- rd integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.W.S";  break;
			case 0b00001: instr.mnemonic = "FCVT.WU.S"; break;
			case 0b00010: instr.mnemonic = "FCVT.L.S";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.LU.S"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1101000: // FCVT.S.W/WU/L/LU -- rs1 integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.S.W";  break;
			case 0b00001: instr.mnemonic = "FCVT.S.WU"; break;
			case 0b00010: instr.mnemonic = "FCVT.S.L";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.S.LU"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1110000: instr.mnemonic = (funct3 == 0b001) ? "FCLASS.S" : "FMV.X.W"; break;
		case 0b1111000: instr.mnemonic = "FMV.W.X"; break;
		}
		break;

	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_F(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	// The decoder already established mstatus.FS is not Off, so the unit is
	// on and this instruction is about to touch FP state. Mark it Dirty so a
	// supervisor knows there is something to save on a context switch.
	vcommon::mark_fp_dirty(regs);

	if (instr.opcode == 0b0000111) { // FLW -- rs1 is an integer base register
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t paddr;
		if (!translate_or_trap(regs, mem, addr, AccessType::Load, paddr)) return;
		regs.write_f(instr.rd, f64_from_bits(box_f32(mem.read32(paddr))));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b0100111) { // FSW -- raw low bits, no unboxing needed
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t paddr;
		if (!translate_or_trap(regs, mem, addr, AccessType::Store, paddr)) return;
		mem.write32(paddr, (uint32_t)bits_from_f64(regs.read_f(instr.rs2)));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b1000011 || instr.opcode == 0b1000111 ||
	    instr.opcode == 0b1001011 || instr.opcode == 0b1001111) { // FMADD/FMSUB/FNMSUB/FNMADD
		bool negate_c = (instr.opcode == 0b1000111 || instr.opcode == 0b1001111);
		bool negate_a = (instr.opcode == 0b1001011 || instr.opcode == 0b1001111);
		float a = read_f32_reg(regs, instr.rs1), b = read_f32_reg(regs, instr.rs2), c = read_f32_reg(regs, instr.rs3);
		if (negate_a) a = -a;
		if (negate_c) c = -c;
		write_f32_reg(regs, instr.rd, fp_fma(a, b, c, instr.funct3, regs));
		regs.set_pc(pc + instr.length);
		return;
	}

	// Everything else is OP-FP; funct7 re-selects the operation the same way
	// decode_f() did to pick its mnemonic.
	switch (instr.funct7) {
	case 0b0000000: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '+', instr.funct3, regs)); break; // FADD.S
	case 0b0000100: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '-', instr.funct3, regs)); break; // FSUB.S
	case 0b0001000: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '*', instr.funct3, regs)); break; // FMUL.S
	case 0b0001100: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '/', instr.funct3, regs)); break; // FDIV.S
	case 0b0101100: write_f32_reg(regs, instr.rd, fp_sqrt(read_f32_reg(regs, instr.rs1), instr.funct3, regs)); break; // FSQRT.S

	case 0b0010000: write_f32_reg(regs, instr.rd, fsgnj_f32(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3)); break; // FSGNJ/N/X.S
	case 0b0010100: write_f32_reg(regs, instr.rd, fminmax(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3 == 0b001, regs)); break; // FMIN/FMAX.S

	case 0b1010000: regs.write_x(instr.rd, fcompare(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3, regs)); break; // FLE/FLT/FEQ.S

	case 0b1110000: // rs1 F -> rd integer: FMV.X.W (raw bits) / FCLASS.S
		if (instr.funct3 == 0b001) regs.write_x(instr.rd, fclassify(read_f32_reg(regs, instr.rs1)));
		else regs.write_x(instr.rd, sext32((uint32_t)bits_from_f64(regs.read_f(instr.rs1))));
		break;
	case 0b1111000: // FMV.W.X -- rd F, rs1 integer, raw bit move
		regs.write_f(instr.rd, f64_from_bits(box_f32((uint32_t)regs.read_x(instr.rs1))));
		break;

	case 0b1100000: { // FCVT.W/WU/L/LU.S -- rd integer; rs2 selects which
		double v = (double)read_f32_reg(regs, instr.rs1);
		clear_fp_exceptions();
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		// volatile: see ext_fp_common.hpp's fp_binop comment -- without it
		// GCC can reorder the conversion past the fetestexcept() below,
		// silently dropping NX.
		volatile uint64_t result = 0;
		switch (instr.rs2) {
		case 0b00000: result = sext32((uint32_t)fcvt_to_i32(v, regs)); break; // FCVT.W.S
		case 0b00001: result = sext32(fcvt_to_u32(v, regs)); break;           // FCVT.WU.S -- still sign-extended per spec
		case 0b00010: result = (uint64_t)fcvt_to_i64(v, regs); break;         // FCVT.L.S
		case 0b00011: result = fcvt_to_u64(v, regs); break;                   // FCVT.LU.S
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b1101000: { // FCVT.S.W/WU/L/LU -- rd F, rs1 integer. Converts straight
		// to float rather than via a double intermediate, to avoid double rounding.
		uint64_t xv = regs.read_x(instr.rs1);
		clear_fp_exceptions();
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		volatile float fv = 0;
		switch (instr.rs2) {
		case 0b00000: fv = (float)(int32_t)xv; break;
		case 0b00001: fv = (float)(uint32_t)xv; break;
		case 0b00010: fv = (float)(int64_t)xv; break;
		case 0b00011: fv = (float)xv; break;
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		write_f32_reg(regs, instr.rd, fv);
		break;
	}

	default:
		break; // the decoder gates unrecognized combinations to illegal first
	}

	regs.set_pc(pc + instr.length);
}
