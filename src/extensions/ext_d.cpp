// D: double-precision floating point. Uses the host's real IEEE-754
// hardware via <cfenv> rather than a from-scratch soft-float library --
// see ext_fp_common.hpp for the shared rounding-mode, exception-flag and
// NaN-canonicalisation plumbing that both F and D sit on.
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
// The f registers are 64 bits wide, so double values need no boxing: unlike
// F's single-precision, these read and write regs.read_f/write_f directly.
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

void RiscvCore::exec_D(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	// The decoder already established mstatus.FS is not Off, so the unit is
	// on and this instruction is about to touch FP state. Mark it Dirty so a
	// supervisor knows there is something to save on a context switch.
	vcommon::mark_fp_dirty(regs);

	if (instr.opcode == 0b0000111) { // FLD -- rs1 is an integer base register
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t paddr;
		if (!translate_or_trap(regs, mem, addr, AccessType::Load, paddr)) return;
		regs.write_f(instr.rd, f64_from_bits(mem.read64(paddr)));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b0100111) { // FSD -- raw bits, no canonicalization needed
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t paddr;
		if (!translate_or_trap(regs, mem, addr, AccessType::Store, paddr)) return;
		mem.write64(paddr, bits_from_f64(regs.read_f(instr.rs2)));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b1000011 || instr.opcode == 0b1000111 ||
	    instr.opcode == 0b1001011 || instr.opcode == 0b1001111) { // FMADD/FMSUB/FNMSUB/FNMADD
		bool negate_c = (instr.opcode == 0b1000111 || instr.opcode == 0b1001111);
		bool negate_a = (instr.opcode == 0b1001011 || instr.opcode == 0b1001111);
		double a = regs.read_f(instr.rs1), b = regs.read_f(instr.rs2), c = regs.read_f(instr.rs3);
		if (negate_a) a = -a;
		if (negate_c) c = -c;
		regs.write_f(instr.rd, fp_fma(a, b, c, instr.funct3, regs));
		regs.set_pc(pc + instr.length);
		return;
	}

	// Everything else is OP-FP; funct7 re-selects the operation the same way
	// decode_d() did to pick its mnemonic.
	switch (instr.funct7) {
	case 0b0000001: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '+', instr.funct3, regs)); break; // FADD.D
	case 0b0000101: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '-', instr.funct3, regs)); break; // FSUB.D
	case 0b0001001: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '*', instr.funct3, regs)); break; // FMUL.D
	case 0b0001101: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '/', instr.funct3, regs)); break; // FDIV.D
	case 0b0101101: regs.write_f(instr.rd, fp_sqrt(regs.read_f(instr.rs1), instr.funct3, regs)); break; // FSQRT.D

	case 0b0010001: regs.write_f(instr.rd, fsgnj_f64(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3)); break; // FSGNJ/N/X.D
	case 0b0010101: regs.write_f(instr.rd, fminmax(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3 == 0b001, regs)); break; // FMIN/FMAX.D

	case 0b1010001: regs.write_x(instr.rd, fcompare(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3, regs)); break; // FLE/FLT/FEQ.D

	case 0b1110001: // rs1 D -> rd integer: FMV.X.D (RV64 only) / FCLASS.D
		if (instr.funct3 == 0b001) regs.write_x(instr.rd, fclassify(regs.read_f(instr.rs1)));
		else regs.write_x(instr.rd, bits_from_f64(regs.read_f(instr.rs1)));
		break;
	case 0b1111001: // FMV.D.X (RV64 only)
		regs.write_f(instr.rd, f64_from_bits(regs.read_x(instr.rs1)));
		break;

	case 0b1100001: { // FCVT.W/WU/L/LU.D -- rd integer; rs2 selects which
		double v = regs.read_f(instr.rs1);
		clear_fp_exceptions();
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		// volatile: see ext_fp_common.hpp's fp_binop comment -- without it
		// GCC can reorder the conversion past the fetestexcept() below,
		// silently dropping NX.
		volatile uint64_t result = 0;
		switch (instr.rs2) {
		case 0b00000: result = sext32((uint32_t)fcvt_to_i32(v, regs)); break; // FCVT.W.D
		case 0b00001: result = sext32(fcvt_to_u32(v, regs)); break;           // FCVT.WU.D -- still sign-extended per spec
		case 0b00010: result = (uint64_t)fcvt_to_i64(v, regs); break;         // FCVT.L.D
		case 0b00011: result = fcvt_to_u64(v, regs); break;                   // FCVT.LU.D
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b1101001: { // FCVT.D.W/WU/L/LU -- rd D, rs1 integer
		uint64_t xv = regs.read_x(instr.rs1);
		clear_fp_exceptions();
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		volatile double dv = 0;
		switch (instr.rs2) {
		case 0b00000: dv = (double)(int32_t)xv; break;
		case 0b00001: dv = (double)(uint32_t)xv; break;
		case 0b00010: dv = (double)(int64_t)xv; break;
		case 0b00011: dv = (double)xv; break;
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		regs.write_f(instr.rd, dv);
		break;
	}

	case 0b0100000: { // FCVT.S.D -- narrow double to single
		double a = regs.read_f(instr.rs1);
		clear_fp_exceptions();
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		volatile float result = (float)a;
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		float rv = result;
		if (std::isnan(rv)) rv = canonical_nan<float>();
		write_f32_reg(regs, instr.rd, rv);
		break;
	}
	case 0b0100001: { // FCVT.D.S -- widen single to double (exact: always representable, no rounding)
		float a = read_f32_reg(regs, instr.rs1);
		double result = std::isnan(a) ? canonical_nan<double>() : (double)a;
		regs.write_f(instr.rd, result);
		break;
	}

	default:
		break; // the decoder gates unrecognized combinations to illegal first
	}

	regs.set_pc(pc + instr.length);
}
