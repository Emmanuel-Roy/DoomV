// OPFVV/OPFVF vector floating-point: arithmetic, sign-injection, min/max,
// compares, scalar<->element moves/merge, convert, sqrt/rsqrt7/rec7/class,
// the fused multiply-add family, widening arithmetic/FMA, and reductions.
// Reuses the exact same host-float machinery (ext_fp_common.hpp) the
// scalar F/D extension uses -- element width is always exactly float or
// double, so there's no separate soft-float path needed here either.
//
// Only SEW in {32,64} is meaningful for FP -- this project doesn't
// implement Zvfh (FP16), so any other current SEW makes every FP vector op
// a no-op, the same permissive-illegal stance taken throughout the decoder
// for reserved encodings.
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include "ext_fp_common.hpp"
#include <cstdint>
#include <cmath>

using namespace vcommon;

namespace {

// Element accessors that dispatch on SEW (32 vs 64) so the rest of this
// file can stay width-generic -- mirrors read_velem/write_velem but
// through the NaN-boxing-aware F register helpers instead of raw ints.
double read_felem(Registers &regs, int base, int sew, uint64_t i)
{
	if (sew == 64) return f64_from_bits(read_velem(regs, base, 64, i));
	return (double)f32_from_bits((uint32_t)read_velem(regs, base, 32, i));
}
void write_felem(Registers &regs, int base, int sew, uint64_t i, double v)
{
	if (sew == 64) write_velem(regs, base, 64, i, bits_from_f64(v));
	else write_velem(regs, base, 32, i, bits_from_f32((float)v));
}

double fbinop_d(double a, double b, char op, uint8_t rm, Registers &regs) { return fp_binop(a, b, op, rm, regs); }
float  fbinop_f(float a, float b, char op, uint8_t rm, Registers &regs) { return fp_binop(a, b, op, rm, regs); }

// Width-generic wrapper for fp_binop/fp_sqrt/fp_fma: computes at the real
// `float`/`double` host type per sew, always returning/taking double (the
// float case round-trips through `float` for correct single-precision
// rounding, matching read_f32_reg/write_f32_reg's role in ext_fd.cpp).
double gbinop(double a, double b, char op, uint8_t rm, int sew, Registers &regs)
{
	return (sew == 64) ? fbinop_d(a, b, op, rm, regs) : (double)fbinop_f((float)a, (float)b, op, rm, regs);
}
double gsqrt(double a, uint8_t rm, int sew, Registers &regs)
{
	return (sew == 64) ? fp_sqrt(a, rm, regs) : (double)fp_sqrt((float)a, rm, regs);
}
double gfma(double a, double b, double c, uint8_t rm, int sew, Registers &regs)
{
	return (sew == 64) ? fp_fma(a, b, c, rm, regs) : (double)fp_fma((float)a, (float)b, (float)c, rm, regs);
}
uint64_t gcompare(double a, double b, uint8_t funct3, int sew, Registers &regs)
{
	return (sew == 64) ? fcompare(a, b, funct3, regs) : fcompare((float)a, (float)b, funct3, regs);
}
double gminmax(double a, double b, bool is_max, int sew, Registers &regs)
{
	return (sew == 64) ? fminmax(a, b, is_max, regs) : (double)fminmax((float)a, (float)b, is_max, regs);
}
double gsgnj(double a, double b, uint8_t funct3, int sew)
{
	return (sew == 64) ? fsgnj_f64(a, b, funct3) : (double)fsgnj_f32((float)a, (float)b, funct3);
}
uint64_t gclassify(double v, int sew)
{
	return (sew == 64) ? fclassify(v) : fclassify((float)v);
}

} // namespace

namespace vcommon {

void exec_v_fp(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	uint8_t funct6 = op_v_funct6(instr.funct7);
	bool vm = op_v_vm(instr.funct7);
	uint8_t rm = regs.get_frm(); // vector FP ops always use the dynamic (fcsr-configured) rounding mode -- there's no per-instruction rm field
	bool is_vv = (instr.funct3 == 0b001);

	if (sew != 32 && sew != 64) return; // no FP16 support -- reserved for this SEW

	// Second operand: vs1[i] (.vv) or the scalar f-register rs1 (.vf).
	auto op2 = [&](uint64_t i) -> double {
		return is_vv ? read_felem(regs, instr.rs1, sew, i) : (sew == 64 ? regs.read_f(instr.rs1) : (double)read_f32_reg(regs, instr.rs1));
	};

	// funct6==0x10 family: vfmv.f.s (vv-space: rd(F) = vs2[0]) / vfmv.s.f (vf-space: vd[0] = rs1(F), rest undisturbed)
	if (funct6 == 0x10) {
		if (is_vv) {
			double v = read_felem(regs, instr.rs2, sew, 0);
			if (sew == 64) regs.write_f(instr.rd, v); else write_f32_reg(regs, instr.rd, (float)v);
		} else if (vl > 0) {
			double v = (sew == 64) ? regs.read_f(instr.rs1) : (double)read_f32_reg(regs, instr.rs1);
			write_felem(regs, instr.rd, sew, 0, v);
		}
		return;
	}

	if (!is_vv && funct6 == 0x17) { // vfmerge.vfm (vm=0) / vfmv.v.f (vm=1), .vf only -- unconditional over vl
		if (!vm) {
			for_each(regs, vl, [&](uint64_t i) {
				double r = mask_bit(regs, i) ? op2(i) : read_felem(regs, instr.rs2, sew, i);
				write_felem(regs, instr.rd, sew, i, r);
			});
		} else {
			for_each(regs, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, op2(i)); });
		}
		return;
	}

	if (!is_vv && (funct6 == 0x0e || funct6 == 0x0f)) { // vfslide1up.vf / vfslide1down.vf
		double scalar = (sew == 64) ? regs.read_f(instr.rs1) : (double)read_f32_reg(regs, instr.rs1);
		if (funct6 == 0x0e) {
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_felem(regs, instr.rd, sew, i, (i == 0) ? scalar : read_felem(regs, instr.rs2, sew, i - 1));
			});
		} else {
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_felem(regs, instr.rd, sew, i, (i + 1 < vl) ? read_felem(regs, instr.rs2, sew, i + 1) : scalar);
			});
		}
		return;
	}

	if (is_vv && (funct6 == 0x01 || funct6 == 0x03 || funct6 == 0x05 || funct6 == 0x07)) { // reductions
		double acc = read_felem(regs, instr.rs1, sew, 0);
		bool is_min = (funct6 == 0x05), is_max = (funct6 == 0x07);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			double v = read_felem(regs, instr.rs2, sew, i);
			if (is_min) acc = gminmax(acc, v, false, sew, regs);
			else if (is_max) acc = gminmax(acc, v, true, sew, regs);
			else acc = gbinop(acc, v, '+', rm, sew, regs); // vfredusum/vfredosum -- ordered-vs-unordered is a
			// non-observable distinction for a single-threaded, non-reassociating sequential implementation like this one.
		});
		if (vl > 0) write_felem(regs, instr.rd, sew, 0, acc);
		return;
	}

	if (is_vv && funct6 == 0x12) { // VFUNARY0: convert family, vs1 field selects
		uint8_t sub = instr.rs1;
		if (sub != 0x00 && sub != 0x01 && sub != 0x02 && sub != 0x03 && sub != 0x06 && sub != 0x07) return;
		bool to_int = (sub == 0x00 || sub == 0x01 || sub == 0x06 || sub == 0x07);
		bool is_unsigned = (sub == 0x00 || sub == 0x06);
		bool force_rtz = (sub == 0x06 || sub == 0x07);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			clear_fp_exceptions();
			int old_round = std::fegetround();
			std::fesetround(force_rtz ? FE_TOWARDZERO : host_round_mode(0b111, regs.get_frm()));
			// volatile results: see ext_fp_common.hpp's fp_binop comment --
			// without it GCC can reorder the actual conversion past the
			// fetestexcept() below, silently dropping NX.
			if (to_int) {
				double src = read_felem(regs, instr.rs2, sew, i);
				volatile uint64_t xr = is_unsigned
					? (sew == 64 ? fcvt_to_u64(src, regs) : (uint64_t)fcvt_to_u32(src, regs))
					: (sew == 64 ? (uint64_t)fcvt_to_i64(src, regs) : (uint64_t)(int64_t)fcvt_to_i32(src, regs));
				std::fesetround(old_round);
				regs.or_fflags(collect_fflags());
				write_velem(regs, instr.rd, sew, i, xr & elem_mask(sew));
			} else {
				double r0 = (sub == 0x02) ? (double)read_velem(regs, instr.rs2, sew, i) // f.xu
				                          : (double)sext_elem(read_velem(regs, instr.rs2, sew, i), sew); // f.x
				volatile double r = (sew == 32) ? (double)(float)r0 : r0; // round through the actual target precision
				std::fesetround(old_round);
				regs.or_fflags(collect_fflags());
				write_felem(regs, instr.rd, sew, i, r);
			}
		});
		return;
	}

	if (is_vv && funct6 == 0x13) { // VFUNARY1: vfsqrt.v / vfrsqrt7.v / vfrec7.v / vfclass.v, vs1 field selects
		uint8_t sub = instr.rs1;
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			double a = read_felem(regs, instr.rs2, sew, i);
			switch (sub) {
			case 0x00: write_felem(regs, instr.rd, sew, i, gsqrt(a, rm, sew, regs)); break;
			case 0x04: write_felem(regs, instr.rd, sew, i, gsqrt(1.0, rm, sew, regs) / gsqrt(a, rm, sew, regs)); break; // vfrsqrt7 (7-bit estimate) -- approximated with the exact value, always at least as accurate as the spec requires
			case 0x05: write_felem(regs, instr.rd, sew, i, gbinop(1.0, a, '/', rm, sew, regs)); break; // vfrec7 (7-bit estimate) -- same approximation
			case 0x10: write_velem(regs, instr.rd, sew, i, gclassify(a, sew)); break;
			}
		});
		return;
	}

	// Everything below is regular per-element arithmetic/compare shared by
	// both .vv and .vf, keyed on funct6.
	switch (funct6) {
	case 0x00: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gbinop(read_felem(regs, instr.rs2, sew, i), op2(i), '+', rm, sew, regs)); }); break;
	case 0x02: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gbinop(read_felem(regs, instr.rs2, sew, i), op2(i), '-', rm, sew, regs)); }); break;
	case 0x27: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gbinop(op2(i), read_felem(regs, instr.rs2, sew, i), '-', rm, sew, regs)); }); break; // vfrsub.vf
	case 0x24: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gbinop(read_felem(regs, instr.rs2, sew, i), op2(i), '*', rm, sew, regs)); }); break;
	case 0x20: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gbinop(read_felem(regs, instr.rs2, sew, i), op2(i), '/', rm, sew, regs)); }); break;
	case 0x21: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gbinop(op2(i), read_felem(regs, instr.rs2, sew, i), '/', rm, sew, regs)); }); break; // vfrdiv.vf
	case 0x08: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gsgnj(read_felem(regs, instr.rs2, sew, i), op2(i), 0b000, sew)); }); break;
	case 0x09: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gsgnj(read_felem(regs, instr.rs2, sew, i), op2(i), 0b001, sew)); }); break;
	case 0x0a: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gsgnj(read_felem(regs, instr.rs2, sew, i), op2(i), 0b010, sew)); }); break;
	case 0x04: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gminmax(read_felem(regs, instr.rs2, sew, i), op2(i), false, sew, regs)); }); break;
	case 0x06: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, sew, i, gminmax(read_felem(regs, instr.rs2, sew, i), op2(i), true, sew, regs)); }); break;
	case 0x18: for_each_active(regs, vm, vl, [&](uint64_t i) { set_mask_bit(regs, instr.rd, i, gcompare(read_felem(regs, instr.rs2, sew, i), op2(i), 0b010, sew, regs)); }); break; // vmfeq
	case 0x19: for_each_active(regs, vm, vl, [&](uint64_t i) { set_mask_bit(regs, instr.rd, i, gcompare(read_felem(regs, instr.rs2, sew, i), op2(i), 0b000, sew, regs)); }); break; // vmfle
	case 0x1b: for_each_active(regs, vm, vl, [&](uint64_t i) { set_mask_bit(regs, instr.rd, i, gcompare(read_felem(regs, instr.rs2, sew, i), op2(i), 0b001, sew, regs)); }); break; // vmflt
	case 0x1c: for_each_active(regs, vm, vl, [&](uint64_t i) { set_mask_bit(regs, instr.rd, i, !gcompare(read_felem(regs, instr.rs2, sew, i), op2(i), 0b010, sew, regs)); }); break; // vmfne = !eq (NaN still compares "not equal" -> true, matching spec: unordered counts as "not equal")
	case 0x1d: for_each_active(regs, vm, vl, [&](uint64_t i) { set_mask_bit(regs, instr.rd, i, gcompare(op2(i), read_felem(regs, instr.rs2, sew, i), 0b001, sew, regs)); }); break; // vmfgt.vf = vs2 < scalar reversed
	case 0x1f: for_each_active(regs, vm, vl, [&](uint64_t i) { set_mask_bit(regs, instr.rd, i, gcompare(op2(i), read_felem(regs, instr.rs2, sew, i), 0b000, sew, regs)); }); break; // vmfge.vf

	case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: { // FMA family
		bool macc_family = (funct6 >> 2) & 1;
		uint8_t neg = funct6 & 0x3;
		bool negate_a = (neg == 0b01 || neg == 0b11);
		bool negate_c = (neg == 0b01 || neg == 0b10);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			double old_vd = read_felem(regs, instr.rd, sew, i);
			double vs2 = read_felem(regs, instr.rs2, sew, i);
			double o2 = op2(i);
			double a = macc_family ? o2 : old_vd;
			double b = macc_family ? vs2 : o2;
			double c = macc_family ? old_vd : vs2;
			if (negate_a) a = -a;
			if (negate_c) c = -c;
			write_felem(regs, instr.rd, sew, i, gfma(a, b, c, rm, sew, regs));
		});
		break;
	}

	default:
		if (funct6 >= 0x30 && funct6 <= 0x3f) { // widening add/sub/mul/FMA and vfwredusum/vfwredosum -- narrow(32)->wide(64) only
			if (sew != 32) return;
			bool op2_is_wide = (funct6 == 0x34 || funct6 == 0x36);
			auto read_vs2w = [&](uint64_t i) -> double {
				return op2_is_wide ? f64_from_bits(read_velem(regs, instr.rs2, 64, i))
				                    : (double)f32_from_bits((uint32_t)read_velem(regs, instr.rs2, 32, i));
			};
			auto narrow2 = [&](uint64_t i) -> double {
				return is_vv ? (double)f32_from_bits((uint32_t)read_velem(regs, instr.rs1, 32, i)) : (double)read_f32_reg(regs, instr.rs1);
			};
			if (funct6 == 0x31 || funct6 == 0x33) { // vfwredusum.vs / vfwredosum.vs (OPFVV only)
				double acc = f64_from_bits(read_velem(regs, instr.rs1, 64, 0));
				for_each_active(regs, vm, vl, [&](uint64_t i) {
					acc = fp_binop(acc, (double)f32_from_bits((uint32_t)read_velem(regs, instr.rs2, 32, i)), '+', rm, regs);
				});
				if (vl > 0) write_velem(regs, instr.rd, 64, 0, bits_from_f64(acc));
				return;
			}
			switch (funct6) {
			case 0x30: case 0x34: for_each_active(regs, vm, vl, [&](uint64_t i) { write_velem(regs, instr.rd, 64, i, bits_from_f64(fp_binop(read_vs2w(i), narrow2(i), '+', rm, regs))); }); break;
			case 0x32: case 0x36: for_each_active(regs, vm, vl, [&](uint64_t i) { write_velem(regs, instr.rd, 64, i, bits_from_f64(fp_binop(read_vs2w(i), narrow2(i), '-', rm, regs))); }); break;
			case 0x38: for_each_active(regs, vm, vl, [&](uint64_t i) { write_velem(regs, instr.rd, 64, i, bits_from_f64(fp_binop(read_vs2w(i), narrow2(i), '*', rm, regs))); }); break;
			case 0x3c: case 0x3d: case 0x3e: case 0x3f: { // vfwmacc/vfwnmacc/vfwmsac/vfwnmsac (macc-family only, no widening madd-family)
				uint8_t neg = funct6 & 0x3;
				bool negate_a = (neg == 0b01 || neg == 0b11);
				bool negate_c = (neg == 0b01 || neg == 0b10);
				for_each_active(regs, vm, vl, [&](uint64_t i) {
					double old_vd = f64_from_bits(read_velem(regs, instr.rd, 64, i));
					double a = narrow2(i), b = read_vs2w(i), c = old_vd;
					if (negate_a) a = -a;
					if (negate_c) c = -c;
					write_velem(regs, instr.rd, 64, i, bits_from_f64(fp_fma(a, b, c, rm, regs)));
				});
				break;
			}
			}
		}
		break;
	}
}

} // namespace vcommon
