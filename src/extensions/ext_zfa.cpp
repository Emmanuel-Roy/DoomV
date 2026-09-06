// Zfa: additional floating-point instructions.
//
// Five small families, all in OP-FP, all sharing funct7 values with base
// F/D and separated by a secondary field. That sharing is the whole reason
// this file needs care -- every one of these sits next to an instruction it
// must not be confused with:
//
//   funct7 0x78 / 0x79, rs2==1     fli.s / fli.d      (rs2==0 is FMV.W.X/FMV.D.X)
//   funct7 0x14 / 0x15, funct3 2/3 fminm / fmaxm      (funct3 0/1 is FMIN/FMAX)
//   funct7 0x20 / 0x21, rs2 4/5    fround / froundnx  (rs2 1/0 is FCVT.S.D/FCVT.D.S)
//   funct7 0x61,        rs2==8     fcvtmod.w.d        (rs2==0 is FCVT.W.D)
//   funct7 0x50 / 0x51, funct3 4/5 fleq / fltq        (funct3 0/1/2 is FLE/FLT/FEQ)
//
// Encodings and the fli constant table both came out of the assembler, not
// from memory. The table was checked in the forward direction: each of the
// 32 values below was assembled as `fli.d fa0, <value>` and confirmed to
// produce consecutive indices 0..31. A table like this is exactly what a
// remembered opcode listing gets subtly wrong -- the entries are not
// uniformly spaced, and indices 2..7 are powers of two while 8..15 are not.
//
// Three semantic details distinguish these from their base-ISA neighbours,
// and all three are the entire point of the respective instruction:
//
//   * fminm/fmaxm return the canonical NaN if *either* operand is NaN.
//     Base FMIN/FMAX return the non-NaN operand instead. Implementing
//     fminm as fmin would be right in every case except the one it exists
//     for.
//
//   * fleq/fltq are the *quiet* comparisons: they raise invalid only for a
//     signalling NaN, where FLE/FLT raise it for any NaN. Same result
//     value, different exception behaviour -- so a test that only checks
//     the result cannot tell them apart, and this file's test checks fflags.
//
//   * fcvtmod.w.d wraps modulo 2^32 instead of saturating. Every other
//     float-to-int conversion in RISC-V clamps out-of-range inputs to the
//     destination's extreme; this one deliberately does not, and NaN and
//     infinity produce 0 rather than the saturated value.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "ext_fp_common.hpp"
#include "ext_xstate.hpp"
#include <cfenv>
#include <cmath>
#include <cstdint>

namespace {

// The fli constant table, as raw double bits. Index 1 is the smallest
// positive *normal* number, not the smallest subnormal; 30 and 31 are +inf
// and the canonical quiet NaN.
const uint64_t FLI_D[32] = {
	0xBFF0000000000000ull, // -1.0
	0x0010000000000000ull, // min normal, 2^-1022
	0x3EF0000000000000ull, // 2^-16
	0x3F00000000000000ull, // 2^-15
	0x3F70000000000000ull, // 2^-8
	0x3F80000000000000ull, // 2^-7
	0x3FB0000000000000ull, // 2^-4
	0x3FC0000000000000ull, // 2^-3
	0x3FD0000000000000ull, // 0.25
	0x3FD4000000000000ull, // 0.3125
	0x3FD8000000000000ull, // 0.375
	0x3FDC000000000000ull, // 0.4375
	0x3FE0000000000000ull, // 0.5
	0x3FE4000000000000ull, // 0.625
	0x3FE8000000000000ull, // 0.75
	0x3FEC000000000000ull, // 0.875
	0x3FF0000000000000ull, // 1.0
	0x3FF4000000000000ull, // 1.25
	0x3FF8000000000000ull, // 1.5
	0x3FFC000000000000ull, // 1.75
	0x4000000000000000ull, // 2.0
	0x4004000000000000ull, // 2.5
	0x4008000000000000ull, // 3.0
	0x4010000000000000ull, // 4.0
	0x4020000000000000ull, // 8.0
	0x4030000000000000ull, // 16.0
	0x4060000000000000ull, // 128.0
	0x4070000000000000ull, // 256.0
	0x40E0000000000000ull, // 32768.0
	0x40F0000000000000ull, // 65536.0
	0x7FF0000000000000ull, // +inf
	0x7FF8000000000000ull, // canonical quiet NaN
};

// Same values in single precision. Not derived from FLI_D by conversion:
// index 1 is the smallest normal *of this format* (2^-126, not the
// double's 2^-1022 rounded), which a conversion would get wrong.
const uint32_t FLI_S[32] = {
	0xBF800000u, // -1.0
	0x00800000u, // min normal, 2^-126
	0x37800000u, // 2^-16
	0x38000000u, // 2^-15
	0x3B800000u, // 2^-8
	0x3C000000u, // 2^-7
	0x3D800000u, // 2^-4
	0x3E000000u, // 2^-3
	0x3E800000u, // 0.25
	0x3EA00000u, // 0.3125
	0x3EC00000u, // 0.375
	0x3EE00000u, // 0.4375
	0x3F000000u, // 0.5
	0x3F200000u, // 0.625
	0x3F400000u, // 0.75
	0x3F600000u, // 0.875
	0x3F800000u, // 1.0
	0x3FA00000u, // 1.25
	0x3FC00000u, // 1.5
	0x3FE00000u, // 1.75
	0x40000000u, // 2.0
	0x40200000u, // 2.5
	0x40400000u, // 3.0
	0x40800000u, // 4.0
	0x41000000u, // 8.0
	0x41800000u, // 16.0
	0x43000000u, // 128.0
	0x43800000u, // 256.0
	0x47000000u, // 32768.0
	0x47800000u, // 65536.0
	0x7F800000u, // +inf
	0x7FC00000u, // canonical quiet NaN
};

// Round to an integral value in the same format, honouring the instruction's
// rounding mode. fround leaves the inexact flag alone; froundnx sets it when
// the value actually changed. std::nearbyint follows the host rounding mode
// (which host_round_mode has already set) and, unlike std::rint, does not
// itself raise inexact -- so the two variants differ only by the explicit
// flag below rather than by which host function is called.
template <typename T>
T fround_impl(T a, bool set_inexact, uint8_t &extra_flags)
{
	if (std::isnan(a) || std::isinf(a) || a == (T)0) return a;
	T r = std::nearbyint(a);
	if (set_inexact && r != a) extra_flags |= 0x01; // NX
	return r;
}

} // namespace

DecodedInstruction Decoder::decode_zfa(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZFA;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	switch (instr.funct7) {
	case 0x78: instr.mnemonic = "FLI.S";  instr.fp_double = false; break;
	case 0x79: instr.mnemonic = "FLI.D";  instr.fp_double = true;  break;
	case 0x14: instr.mnemonic = (instr.funct3 == 2) ? "FMINM.S" : "FMAXM.S"; instr.fp_double = false; break;
	case 0x15: instr.mnemonic = (instr.funct3 == 2) ? "FMINM.D" : "FMAXM.D"; instr.fp_double = true;  break;
	case 0x20: instr.mnemonic = (instr.rs2 == 4) ? "FROUND.S" : "FROUNDNX.S"; instr.fp_double = false; break;
	case 0x21: instr.mnemonic = (instr.rs2 == 4) ? "FROUND.D" : "FROUNDNX.D"; instr.fp_double = true;  break;
	case 0x61: instr.mnemonic = "FCVTMOD.W.D"; instr.fp_double = true; break;
	case 0x50: instr.mnemonic = (instr.funct3 == 4) ? "FLEQ.S" : "FLTQ.S"; instr.fp_double = false; break;
	case 0x51: instr.mnemonic = (instr.funct3 == 4) ? "FLEQ.D" : "FLTQ.D"; instr.fp_double = true;  break;
	}
	return instr;
}

void RiscvCore::exec_ZFA(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint8_t extra_flags = 0;

	switch (instr.funct7) {
	case 0x78: // fli.s -- rs1 is the table index, not a register
		regs.write_f(instr.rd, f64_from_bits(box_f32(FLI_S[instr.rs1])));
		break;

	case 0x79: // fli.d
		regs.write_f(instr.rd, f64_from_bits(FLI_D[instr.rs1]));
		break;

	case 0x14: { // fminm.s / fmaxm.s
		float a = read_f32_reg(regs, instr.rs1), b = read_f32_reg(regs, instr.rs2);
		float r;
		// The difference from FMIN/FMAX: any NaN operand poisons the
		// result rather than being skipped over.
		if (std::isnan(a) || std::isnan(b)) {
			r = canonical_nan<float>();
			if (is_snan_f32(a) || is_snan_f32(b)) extra_flags |= 0x10; // NV
		} else {
			r = fminmax<float>(a, b, instr.funct3 == 3, regs);
		}
		write_f32_reg(regs, instr.rd, r);
		break;
	}

	case 0x15: { // fminm.d / fmaxm.d
		double a = regs.read_f(instr.rs1), b = regs.read_f(instr.rs2);
		double r;
		if (std::isnan(a) || std::isnan(b)) {
			r = canonical_nan<double>();
			if (is_snan_f64(a) || is_snan_f64(b)) extra_flags |= 0x10;
		} else {
			r = fminmax<double>(a, b, instr.funct3 == 3, regs);
		}
		regs.write_f(instr.rd, r);
		break;
	}

	case 0x20: { // fround.s / froundnx.s
		float a = read_f32_reg(regs, instr.rs1);
		if (is_snan_f32(a)) extra_flags |= 0x10;
		clear_fp_exceptions();
		int saved = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		float r = std::isnan(a) ? canonical_nan<float>()
		                        : fround_impl<float>(a, instr.rs2 == 5, extra_flags);
		std::fesetround(saved);
		write_f32_reg(regs, instr.rd, r);
		break;
	}

	case 0x21: { // fround.d / froundnx.d
		double a = regs.read_f(instr.rs1);
		if (is_snan_f64(a)) extra_flags |= 0x10;
		clear_fp_exceptions();
		int saved = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		double r = std::isnan(a) ? canonical_nan<double>()
		                         : fround_impl<double>(a, instr.rs2 == 5, extra_flags);
		std::fesetround(saved);
		regs.write_f(instr.rd, r);
		break;
	}

	case 0x61: { // fcvtmod.w.d -- truncate toward zero, then wrap modulo 2^32
		double a = regs.read_f(instr.rs1);
		int32_t r;
		if (std::isnan(a) || std::isinf(a)) {
			// Unlike every other float-to-int conversion, which
			// saturates, this one yields zero and raises invalid.
			r = 0;
			extra_flags |= 0x10;
		} else {
			double t = std::trunc(a);
			// Out of range still produces the wrapped result -- that is
			// the point of the instruction -- but it raises invalid,
			// and invalid *replaces* inexact rather than joining it.
			// Both halves are easy to miss because the value is right
			// either way; only a test dumping fflags per operation
			// sees them, and the NV-suppresses-NX half was settled by
			// asking spike with an input that is both out of range and
			// non-integral rather than by assuming.
			if (t < -2147483648.0 || t > 2147483647.0) {
				extra_flags |= 0x10; // NV
			} else if (t != a) {
				extra_flags |= 0x01; // NX
			}
			// Wrap rather than clamp, and do the reduction in double
			// arithmetic: fmod is exact for integral doubles, and it
			// keeps the huge inputs this instruction is meant to accept
			// away from an integer cast, which would be undefined for
			// anything past 2^63.
			double m = std::fmod(t, 4294967296.0);
			if (m < 0) m += 4294967296.0;
			r = (int32_t)(uint32_t)m;
		}
		regs.write_x(instr.rd, (uint64_t)(int64_t)r);
		break;
	}

	case 0x50: { // fleq.s / fltq.s -- quiet comparisons
		float a = read_f32_reg(regs, instr.rs1), b = read_f32_reg(regs, instr.rs2);
		// Only a signalling NaN raises invalid here; FLE/FLT would raise
		// it for a quiet NaN too. The result value is the same either way,
		// so this flag is the only observable difference.
		if (is_snan_f32(a) || is_snan_f32(b)) extra_flags |= 0x10;
		bool r = std::isnan(a) || std::isnan(b) ? false
		       : (instr.funct3 == 4 ? (a <= b) : (a < b));
		regs.write_x(instr.rd, r ? 1 : 0);
		break;
	}

	case 0x51: { // fleq.d / fltq.d
		double a = regs.read_f(instr.rs1), b = regs.read_f(instr.rs2);
		if (is_snan_f64(a) || is_snan_f64(b)) extra_flags |= 0x10;
		bool r = std::isnan(a) || std::isnan(b) ? false
		       : (instr.funct3 == 4 ? (a <= b) : (a < b));
		regs.write_x(instr.rd, r ? 1 : 0);
		break;
	}
	}

	if (extra_flags) regs.set_fflags((uint8_t)(regs.get_fflags() | extra_flags));
	vcommon::mark_fp_dirty(regs);
	regs.set_pc(regs.get_pc() + instr.length);
}
