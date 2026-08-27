// F/D extensions: single- and double-precision floating point. Uses the
// host's real IEEE-754 hardware via <cfenv> instead of a from-scratch
// soft-float library.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include "ext_fp_common.hpp"
#include <cfenv>
#include <climits>
#include <cmath>
#include <cstring>

DecodedInstruction Decoder::decode_fd(uint32_t raw_instr, Extension ext) const
{
	DecodedInstruction instr{};
	instr.ext = ext;
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

	int32_t imm_i = (int32_t)raw_instr >> 20;

	int32_t imm_s = (((raw_instr >> 25) & 0x7F) << 5) | ((raw_instr >> 7) & 0x1F);
	if (raw_instr & 0x80000000) imm_s |= 0xFFFFF000;

	switch (opcode) {
	case 0b0000111: // LOAD-FP: FLW (F) / FLD (D) -- rs1 is an integer base register, rd is F/D
		instr.imm = imm_i;
		instr.fp_double = (funct3 == 0b011);
		instr.mnemonic = instr.fp_double ? "FLD" : "FLW";
		break;

	case 0b0100111: // STORE-FP: FSW (F) / FSD (D)
		instr.imm = imm_s;
		instr.fp_double = (funct3 == 0b011);
		instr.mnemonic = instr.fp_double ? "FSD" : "FSW";
		break;

	// Fused multiply-add family (R4-type: rs3 lives at bits[31:27], funct2
	// at bits[26:25] -- both packed into the already-extracted funct7).
	case 0b1000011: instr.rs3 = funct7 >> 2; instr.fp_double = (funct7 & 0x3) == 0b01;
		instr.mnemonic = instr.fp_double ? "FMADD.D" : "FMADD.S"; break;
	case 0b1000111: instr.rs3 = funct7 >> 2; instr.fp_double = (funct7 & 0x3) == 0b01;
		instr.mnemonic = instr.fp_double ? "FMSUB.D" : "FMSUB.S"; break;
	case 0b1001011: instr.rs3 = funct7 >> 2; instr.fp_double = (funct7 & 0x3) == 0b01;
		instr.mnemonic = instr.fp_double ? "FNMSUB.D" : "FNMSUB.S"; break;
	case 0b1001111: instr.rs3 = funct7 >> 2; instr.fp_double = (funct7 & 0x3) == 0b01;
		instr.mnemonic = instr.fp_double ? "FNMADD.D" : "FNMADD.S"; break;

	case 0b1010011: { // OP-FP -- funct7 selects the operation; rs2 doubles as a conversion-type selector for FCVT
		instr.fp_double = (funct7 & 0x1) != 0;
		switch (funct7) {
		case 0b0000000: instr.mnemonic = "FADD.S";  break;
		case 0b0000001: instr.mnemonic = "FADD.D";  break;
		case 0b0000100: instr.mnemonic = "FSUB.S";  break;
		case 0b0000101: instr.mnemonic = "FSUB.D";  break;
		case 0b0001000: instr.mnemonic = "FMUL.S";  break;
		case 0b0001001: instr.mnemonic = "FMUL.D";  break;
		case 0b0001100: instr.mnemonic = "FDIV.S";  break;
		case 0b0001101: instr.mnemonic = "FDIV.D";  break;
		case 0b0101100: instr.mnemonic = "FSQRT.S"; break;
		case 0b0101101: instr.mnemonic = "FSQRT.D"; break;
		case 0b0010000:
			switch (funct3) {
			case 0b000: instr.mnemonic = "FSGNJ.S";  break;
			case 0b001: instr.mnemonic = "FSGNJN.S"; break;
			case 0b010: instr.mnemonic = "FSGNJX.S"; break;
			}
			break;
		case 0b0010001:
			switch (funct3) {
			case 0b000: instr.mnemonic = "FSGNJ.D";  break;
			case 0b001: instr.mnemonic = "FSGNJN.D"; break;
			case 0b010: instr.mnemonic = "FSGNJX.D"; break;
			}
			break;
		case 0b0010100: instr.mnemonic = (funct3 == 0b001) ? "FMAX.S" : "FMIN.S"; break;
		case 0b0010101: instr.mnemonic = (funct3 == 0b001) ? "FMAX.D" : "FMIN.D"; break;
		case 0b1100000: // FCVT.W/WU/L/LU .S -- rd is an integer register
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.W.S";  break;
			case 0b00001: instr.mnemonic = "FCVT.WU.S"; break;
			case 0b00010: instr.mnemonic = "FCVT.L.S";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.LU.S"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1100001: // FCVT.W/WU/L/LU .D
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.W.D";  break;
			case 0b00001: instr.mnemonic = "FCVT.WU.D"; break;
			case 0b00010: instr.mnemonic = "FCVT.L.D";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.LU.D"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1110000: // rs1 F -> rd integer: FMV.X.W (raw bits) / FCLASS.S
			instr.mnemonic = (funct3 == 0b001) ? "FCLASS.S" : "FMV.X.W";
			break;
		case 0b1110001: // rs1 D -> rd integer: FMV.X.D (RV64 only) / FCLASS.D
			if (funct3 == 0b001) {
				instr.mnemonic = "FCLASS.D";
			} else {
				instr.mnemonic = "FMV.X.D";
				if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL;
			}
			break;
		case 0b1010000: // FLE/FLT/FEQ .S -- rd integer
			switch (funct3) {
			case 0b000: instr.mnemonic = "FLE.S"; break;
			case 0b001: instr.mnemonic = "FLT.S"; break;
			case 0b010: instr.mnemonic = "FEQ.S"; break;
			}
			break;
		case 0b1010001: // FLE/FLT/FEQ .D
			switch (funct3) {
			case 0b000: instr.mnemonic = "FLE.D"; break;
			case 0b001: instr.mnemonic = "FLT.D"; break;
			case 0b010: instr.mnemonic = "FEQ.D"; break;
			}
			break;
		case 0b1101000: // FCVT.S.W/WU/L/LU -- rd F, rs1 integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.S.W";  break;
			case 0b00001: instr.mnemonic = "FCVT.S.WU"; break;
			case 0b00010: instr.mnemonic = "FCVT.S.L";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.S.LU"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1101001: // FCVT.D.W/WU/L/LU -- rd D, rs1 integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.D.W";  break;
			case 0b00001: instr.mnemonic = "FCVT.D.WU"; break;
			case 0b00010: instr.mnemonic = "FCVT.D.L";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.D.LU"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1111000: // FMV.W.X -- rd F, rs1 integer, raw bit move
			instr.mnemonic = "FMV.W.X";
			break;
		case 0b1111001: // FMV.D.X (RV64 only)
			instr.mnemonic = "FMV.D.X";
			if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL;
			break;
		case 0b0100000: // FCVT.S.D -- narrows a double to single. Touches both widths, so
			instr.mnemonic = "FCVT.S.D"; // unlike every other F or D op here, this needs *both* flags enabled, not just one.
			if (!Extensions.F || !Extensions.D) instr.ext = Extension::ILLEGAL;
			break;
		case 0b0100001: // FCVT.D.S -- widens a single to double, same both-flags requirement
			instr.mnemonic = "FCVT.D.S";
			if (!Extensions.F || !Extensions.D) instr.ext = Extension::ILLEGAL;
			break;
		}
		break;
	}

	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_FD(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	if (instr.opcode == 0b0000111) { // LOAD-FP: FLW/FLD -- rs1 is an integer base register
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t paddr;
		if (!translate_or_trap(regs, mem, addr, AccessType::Load, paddr)) return;
		if (instr.fp_double) regs.write_f(instr.rd, f64_from_bits(mem.read64(paddr)));
		else regs.write_f(instr.rd, f64_from_bits(box_f32(mem.read32(paddr))));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b0100111) { // STORE-FP: FSW/FSD -- raw low bits, no unboxing/canonicalization needed
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t paddr;
		if (!translate_or_trap(regs, mem, addr, AccessType::Store, paddr)) return;
		if (instr.fp_double) mem.write64(paddr, bits_from_f64(regs.read_f(instr.rs2)));
		else mem.write32(paddr, (uint32_t)bits_from_f64(regs.read_f(instr.rs2)));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b1000011 || instr.opcode == 0b1000111 ||
	    instr.opcode == 0b1001011 || instr.opcode == 0b1001111) { // FMADD/FMSUB/FNMSUB/FNMADD
		bool negate_c = (instr.opcode == 0b1000111 || instr.opcode == 0b1001111);
		bool negate_a = (instr.opcode == 0b1001011 || instr.opcode == 0b1001111);
		if (instr.fp_double) {
			double a = regs.read_f(instr.rs1), b = regs.read_f(instr.rs2), c = regs.read_f(instr.rs3);
			if (negate_a) a = -a;
			if (negate_c) c = -c;
			regs.write_f(instr.rd, fp_fma(a, b, c, instr.funct3, regs));
		} else {
			float a = read_f32_reg(regs, instr.rs1), b = read_f32_reg(regs, instr.rs2), c = read_f32_reg(regs, instr.rs3);
			if (negate_a) a = -a;
			if (negate_c) c = -c;
			write_f32_reg(regs, instr.rd, fp_fma(a, b, c, instr.funct3, regs));
		}
		regs.set_pc(pc + instr.length);
		return;
	}

	// Everything else is OP-FP (0b1010011); instr.funct7 re-selects the
	// exact operation the same way decode_fd() did to pick its mnemonic.
	switch (instr.funct7) {
	case 0b0000000: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '+', instr.funct3, regs)); break; // FADD.S
	case 0b0000001: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '+', instr.funct3, regs)); break; // FADD.D
	case 0b0000100: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '-', instr.funct3, regs)); break; // FSUB.S
	case 0b0000101: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '-', instr.funct3, regs)); break; // FSUB.D
	case 0b0001000: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '*', instr.funct3, regs)); break; // FMUL.S
	case 0b0001001: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '*', instr.funct3, regs)); break; // FMUL.D
	case 0b0001100: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '/', instr.funct3, regs)); break; // FDIV.S
	case 0b0001101: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '/', instr.funct3, regs)); break; // FDIV.D
	case 0b0101100: write_f32_reg(regs, instr.rd, fp_sqrt(read_f32_reg(regs, instr.rs1), instr.funct3, regs)); break; // FSQRT.S
	case 0b0101101: regs.write_f(instr.rd, fp_sqrt(regs.read_f(instr.rs1), instr.funct3, regs)); break; // FSQRT.D

	case 0b0010000: write_f32_reg(regs, instr.rd, fsgnj_f32(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3)); break; // FSGNJ/N/X.S
	case 0b0010001: regs.write_f(instr.rd, fsgnj_f64(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3)); break; // FSGNJ/N/X.D
	case 0b0010100: write_f32_reg(regs, instr.rd, fminmax(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3 == 0b001, regs)); break; // FMIN/FMAX.S
	case 0b0010101: regs.write_f(instr.rd, fminmax(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3 == 0b001, regs)); break; // FMIN/FMAX.D

	case 0b1010000: regs.write_x(instr.rd, fcompare(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3, regs)); break; // FLE/FLT/FEQ.S
	case 0b1010001: regs.write_x(instr.rd, fcompare(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3, regs)); break; // FLE/FLT/FEQ.D

	case 0b1110000: // rs1 F -> rd integer: FMV.X.W (raw bits) / FCLASS.S
		if (instr.funct3 == 0b001) regs.write_x(instr.rd, fclassify(read_f32_reg(regs, instr.rs1)));
		else regs.write_x(instr.rd, sext32((uint32_t)bits_from_f64(regs.read_f(instr.rs1))));
		break;
	case 0b1110001: // rs1 D -> rd integer: FMV.X.D (RV64 only) / FCLASS.D
		if (instr.funct3 == 0b001) regs.write_x(instr.rd, fclassify(regs.read_f(instr.rs1)));
		else regs.write_x(instr.rd, bits_from_f64(regs.read_f(instr.rs1)));
		break;
	case 0b1111000: // FMV.W.X -- rd F, rs1 integer, raw bit move
		regs.write_f(instr.rd, f64_from_bits(box_f32((uint32_t)regs.read_x(instr.rs1))));
		break;
	case 0b1111001: // FMV.D.X (RV64 only)
		regs.write_f(instr.rd, f64_from_bits(regs.read_x(instr.rs1)));
		break;

	case 0b1100000: case 0b1100001: { // FCVT.W/WU/L/LU .S or .D -- rd integer; rs2 (reused) selects which
		double v = instr.fp_double ? regs.read_f(instr.rs1) : (double)read_f32_reg(regs, instr.rs1);
		clear_fp_exceptions();
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		// volatile: see ext_fp_common.hpp's fp_binop comment -- without it
		// GCC can reorder the actual conversion past the fetestexcept()
		// below, silently dropping NX.
		volatile uint64_t result = 0;
		switch (instr.rs2) {
		case 0b00000: result = sext32((uint32_t)fcvt_to_i32(v, regs)); break; // FCVT.W.*
		case 0b00001: result = sext32(fcvt_to_u32(v, regs)); break;          // FCVT.WU.* -- still sign-extended per spec
		case 0b00010: result = (uint64_t)fcvt_to_i64(v, regs); break;       // FCVT.L.*
		case 0b00011: result = fcvt_to_u64(v, regs); break;                 // FCVT.LU.*
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b1101000: { // FCVT.S.W/WU/L/LU -- rd F (single), rs1 integer. Converts straight to float
		// (not via a double intermediate) to avoid a double-rounding step.
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

	case 0b0100000: { // FCVT.S.D -- narrow double to single (needs both F and D, enforced in decode_fd())
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
	case 0b0100001: { // FCVT.D.S -- widen single to double (exact -- always representable, no rounding needed)
		float a = read_f32_reg(regs, instr.rs1);
		double result = std::isnan(a) ? canonical_nan<double>() : (double)a;
		regs.write_f(instr.rd, result);
		break;
	}

	default:
		break; // decoder gates unrecognized funct7/rs2/funct3 combinations to illegal before this is ever called
	}

	regs.set_pc(pc + instr.length);
}
