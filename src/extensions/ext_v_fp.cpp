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
#include "ext_fp16.hpp"
#include "ext_softfloat.hpp"
#include <cstdint>
#include <cmath>

using namespace vcommon;

namespace {

// Element accessors that dispatch on SEW (32 vs 64) so the rest of this
// file can stay width-generic -- mirrors read_velem/write_velem but
// through the NaN-boxing-aware F register helpers instead of raw ints.
// Half elements travel through `double` like the other two widths, and that
// is exact rather than a shortcut: every binary16 value is representable in
// binary64, so widening on the way in and narrowing on the way out loses
// nothing. What matters is that the *arithmetic* happens at binary16 -- see
// the g* wrappers below, which convert back to f16 to compute. Computing in
// double and narrowing once at the end would round twice, and with an
// 11-bit significand that is routine rather than a corner case.
double read_felem(Registers &regs, int base, int sew, uint64_t i)
{
	if (sew == 64) return f64_from_bits(read_velem(regs, base, 64, i));
	if (sew == 16) {
		uint8_t f = 0;
		return f64_from_bits(fp16::h_to_f64_bits((uint16_t)read_velem(regs, base, 16, i), f));
	}
	return (double)f32_from_bits((uint32_t)read_velem(regs, base, 32, i));
}
void write_felem(Registers &regs, int base, int sew, uint64_t i, double v)
{
	if (sew == 64) write_velem(regs, base, 64, i, bits_from_f64(v));
	else if (sew == 16) {
		// Exact by construction: v is the result of an f16 operation, so
		// narrowing it back cannot round. A mode still has to be named.
		uint8_t f = 0;
		write_velem(regs, base, 16, i, fp16::f64_bits_to_h(bits_from_f64(v), 0, f));
	}
	else write_velem(regs, base, 32, i, bits_from_f32((float)v));
}

// The f16 bit pattern of a value that came from an f16 element, and back.
// Exact in both directions for the reason above.
inline uint16_t as_h(double v)
{
	uint8_t f = 0;
	return fp16::f64_bits_to_h(bits_from_f64(v), 0, f);
}
inline double from_h(uint16_t h)
{
	uint8_t f = 0;
	return f64_from_bits(fp16::h_to_f64_bits(h, f));
}

double fbinop_d(double a, double b, char op, uint8_t rm, Registers &regs) { return fp_binop(a, b, op, rm, regs); }
float  fbinop_f(float a, float b, char op, uint8_t rm, Registers &regs) { return fp_binop(a, b, op, rm, regs); }

// Width-generic wrapper for fp_binop/fp_sqrt/fp_fma: computes at the real
// `float`/`double` host type per sew, always returning/taking double (the
// float case round-trips through `float` for correct single-precision
// rounding, matching read_f32_reg/write_f32_reg's role in ext_fd.cpp).
// Every half-precision arm computes with SoftFloat f16 directly. Widening to
// float and narrowing back would round twice, and the lesson of the scalar
// SoftFloat migration is that rounding written twice rounds differently.
double gbinop(double a, double b, char op, uint8_t rm, int sew, Registers &regs)
{
	if (sew == 16) {
		sf::begin(rm, regs.get_frm());
		float16_t x = sf::f16(as_h(a)), y = sf::f16(as_h(b));
		float16_t r;
		if (op == '+') r = f16_add(x, y);
		else if (op == '-') r = f16_sub(x, y);
		else if (op == '*') r = f16_mul(x, y);
		else r = f16_div(x, y);
		sf::end(regs);
		return from_h(sf::bits(r));
	}
	return (sew == 64) ? fbinop_d(a, b, op, rm, regs) : (double)fbinop_f((float)a, (float)b, op, rm, regs);
}
double gsqrt(double a, uint8_t rm, int sew, Registers &regs)
{
	if (sew == 16) {
		sf::begin(rm, regs.get_frm());
		float16_t r = f16_sqrt(sf::f16(as_h(a)));
		sf::end(regs);
		return from_h(sf::bits(r));
	}
	return (sew == 64) ? fp_sqrt(a, rm, regs) : (double)fp_sqrt((float)a, rm, regs);
}
double gfma(double a, double b, double c, uint8_t rm, int sew, Registers &regs)
{
	if (sew == 16) {
		sf::begin(rm, regs.get_frm());
		float16_t r = f16_mulAdd(sf::f16(as_h(a)), sf::f16(as_h(b)), sf::f16(as_h(c)));
		sf::end(regs);
		return from_h(sf::bits(r));
	}
	return (sew == 64) ? fp_fma(a, b, c, rm, regs) : (double)fp_fma((float)a, (float)b, (float)c, rm, regs);
}
uint64_t gcompare(double a, double b, uint8_t funct3, int sew, Registers &regs)
{
	if (sew == 16) {
		// funct3 here is the *scalar* comparison encoding the callers pass
		// -- 0b010 for equality, 0b001 for less-than, 0b000 for
		// less-or-equal -- not the vector funct6, and not the vmf* opcode
		// order. Reversal and negation are the caller's job too: vmfgt
		// swaps its operands and vmfne inverts the result.
		//
		// Routed through f16 rather than reusing the double path because
		// the difference is in the flags, not the answer: SoftFloat's
		// f16_eq is quiet and raises invalid only on a signalling NaN,
		// while f16_lt and f16_le signal on any NaN. That is exactly the
		// rule the architecture wants, so picking the right function gets
		// the flags for free.
		sf::begin(0, regs.get_frm());
		float16_t x = sf::f16(as_h(a)), y = sf::f16(as_h(b));
		bool out;
		switch (funct3) {
		case 0b010: out = f16_eq(x, y); break;   // EQ, quiet
		case 0b001: out = f16_lt(x, y); break;   // LT, signalling
		default:    out = f16_le(x, y); break;   // LE, signalling
		}
		sf::end(regs);
		return out ? 1u : 0u;
	}
	return (sew == 64) ? fcompare(a, b, funct3, regs) : fcompare((float)a, (float)b, funct3, regs);
}
double gminmax(double a, double b, bool is_max, int sew, Registers &regs)
{
	if (sew == 16) {
		// The same not-SoftFloat rules scalar half min/max follows: a quiet
		// NaN is ignored rather than propagated, two NaNs give the canonical
		// NaN, and -0.0 sorts below +0.0.
		const uint16_t x = as_h(a), y = as_h(b);
		if (fp16::h_is_snan(x) || fp16::h_is_snan(y)) regs.or_fflags(0x10);
		const bool xn = fp16::h_is_nan(x), yn = fp16::h_is_nan(y);
		if (xn && yn) return from_h(0x7E00);
		if (xn) return from_h(y);
		if (yn) return from_h(x);
		if (((x | y) & 0x7FFF) == 0) {
			const bool x_neg = (x >> 15) != 0;
			return from_h(is_max ? (x_neg ? y : x) : (x_neg ? x : y));
		}
		const bool lt = f16_lt(sf::f16(x), sf::f16(y));
		return from_h(is_max ? (lt ? y : x) : (lt ? x : y));
	}
	return (sew == 64) ? fminmax(a, b, is_max, regs) : (double)fminmax((float)a, (float)b, is_max, regs);
}
double gsgnj(double a, double b, uint8_t funct3, int sew)
{
	if (sew == 16) {
		const uint16_t x = as_h(a), y = as_h(b);
		uint16_t sign;
		switch (funct3) {
		case 0:  sign = y & 0x8000; break;
		case 1:  sign = (uint16_t)((~y) & 0x8000); break;
		default: sign = (uint16_t)((x ^ y) & 0x8000); break;
		}
		return from_h((uint16_t)((x & 0x7FFF) | sign));
	}
	return (sew == 64) ? fsgnj_f64(a, b, funct3) : (double)fsgnj_f32((float)a, (float)b, funct3);
}
uint64_t gclassify(double v, int sew)
{
	if (sew == 16) {
		const uint16_t h = as_h(v);
		const uint16_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
		const bool neg = (h >> 15) != 0;
		if (e == 0x1F && m == 0)   return neg ? (1u << 0) : (1u << 7);
		if (e == 0x1F)             return (m & 0x200) ? (1u << 9) : (1u << 8);
		if (e == 0 && m == 0)      return neg ? (1u << 3) : (1u << 4);
		if (e == 0)                return neg ? (1u << 2) : (1u << 5);
		return neg ? (1u << 1) : (1u << 6);
	}
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

	// SEW=16 is legal for exactly two instructions, the Zvfhmin conversions
	// vfwcvt.f.f.v and vfncvt.f.f.w (VFUNARY0, vs1 0x0C and 0x14). Half
	// precision arithmetic would need Zvfh, which RVA23 makes an expansion
	// option and this emulator does not implement.
	//
	// Everything else at SEW=16 returns without doing anything, which is
	// the same silent no-op that hid the rest of VFUNARY0 for so long. It
	// is kept deliberately here rather than fixed in passing: making it an
	// illegal instruction is a separate behaviour change, and the honest
	// thing is to say so rather than leave the reader to infer it.
	// Zvfh: SEW=16 is a first-class floating-point width now, not just the
	// two Zvfhmin conversions. It used to fall through to a no-op, which is
	// the worst of the options -- the destination kept a stale value and
	// nothing said the operation had not happened. 895 of riscv-vector-
	// tests 3042 cases are e16, so this was most of that suite.
	if (sew != 16 && sew != 32 && sew != 64) return;

	// Second operand: vs1[i] (.vv) or the scalar f-register rs1 (.vf).
	// The scalar operand of a .vf form comes from an f register, and it has
	// to be read at the element width. Reading a half-precision element as
	// single -- which is what happened for every width below 64 -- does not
	// give a slightly wrong number, it gives an unrelated one, because the
	// register holds a NaN-boxed f16 and the f32 interpretation of that
	// pattern is meaningless. Every .vf form was affected: the compares,
	// sign-injection, min/max and the arithmetic alike.
	auto read_fscalar = [&](int w) -> double {
		if (w == 64) return regs.read_f(instr.rs1);
		if (w == 16) {
			uint8_t f = 0;
			return f64_from_bits(fp16::h_to_f64_bits(
				fp16::unbox_f16(bits_from_f64(regs.read_f(instr.rs1))), f));
		}
		return (double)read_f32_reg(regs, instr.rs1);
	};
	auto op2 = [&](uint64_t i) -> double {
		return is_vv ? read_felem(regs, instr.rs1, sew, i) : read_fscalar(sew);
	};

	// funct6==0x10 family: vfmv.f.s (vv-space: rd(F) = vs2[0]) / vfmv.s.f (vf-space: vd[0] = rs1(F), rest undisturbed)
	if (funct6 == 0x10) {
		if (is_vv) {
			double v = read_felem(regs, instr.rs2, sew, 0);
			if (sew == 64) regs.write_f(instr.rd, v);
			else if (sew == 16) {
				// Written NaN-boxed, the way every half-precision producer
				// leaves an f register.
				uint8_t f = 0;
				regs.write_f(instr.rd, f64_from_bits(fp16::box_f16(
					fp16::f64_bits_to_h(bits_from_f64(v), 0, f))));
			}
			else write_f32_reg(regs, instr.rd, (float)v);
		} else if (vl > 0) {
			write_felem(regs, instr.rd, sew, 0, read_fscalar(sew));
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

	// VFUNARY0's widening (vs1 0x08-0x0F) and narrowing (0x10-0x17) halves.
	// These used to fall off the end of the same-width handler below and
	// return having done nothing -- the destination register kept whatever
	// it held before, silently, which is the same failure Zvbb had.
	//
	// vfwcvt.f.f.v and vfncvt.f.f.w are what Zvfhmin mandates (SEW 16 <-> 32);
	// the rest of the family is base V and is implemented here too, because
	// leaving a subset silently ignored is what caused the problem in the
	// first place.
	//
	// Widening reads at SEW and writes at 2*SEW; narrowing reads the wide
	// operand at 2*SEW and writes at SEW. Half precision only ever appears
	// as the narrow side, and goes through ext_fp16.hpp rather than the host
	// FPU, which has no 16-bit type.
	if (is_vv && funct6 == 0x12 && instr.rs1 >= 0x08) {
		uint8_t sub = instr.rs1;
		bool widening = sub < 0x10;
		int nsew = sew;              // the narrow side
		int wsew = sew * 2;          // the wide side
		if (wsew > 64) return;       // no format wider than 64 bits exists

		// Reading and writing a float element of a width the host has no
		// type for: 16-bit goes through the software conversion, 32 and 64
		// use the host float and double directly.
		auto read_f = [&](int vreg, int esew, uint64_t i) -> double {
			if (esew != 16) return read_felem(regs, vreg, esew, i);
			uint8_t fl = 0;
			double d = f64_from_bits(fp16::h_to_f64_bits((uint16_t)read_velem(regs, vreg, 16, i), fl));
			regs.or_fflags(fl);
			return d;
		};
		auto write_f = [&](int vreg, int esew, uint64_t i, double v, int rm) {
			if (esew != 16) { write_felem(regs, vreg, esew, i, v); return; }
			uint8_t fl = 0;
			uint16_t h = fp16::f64_bits_to_h(bits_from_f64(v), rm, fl);
			regs.or_fflags(fl);
			write_velem(regs, vreg, 16, i, h);
		};

		int rm_default = host_round_mode(0b111, regs.get_frm());

		for_each_active(regs, vm, vl, [&](uint64_t i) {
			clear_fp_exceptions();
			int old_round = std::fegetround();

			if (widening) {
				switch (sub) {
				case 0x08: case 0x09: case 0x0E: case 0x0F: { // int <- float, widened
					// The .rtz forms (0x0E/0x0F) ignore frm entirely.
					std::fesetround((sub >= 0x0E) ? FE_TOWARDZERO : rm_default);
					double src = read_f(instr.rs2, nsew, i);
					bool uns = (sub == 0x08 || sub == 0x0E);
					volatile uint64_t xr = uns
						? (wsew == 64 ? fcvt_to_u64(src, regs) : (uint64_t)fcvt_to_u32(src, regs))
						: (wsew == 64 ? (uint64_t)fcvt_to_i64(src, regs)
						              : (uint64_t)(int64_t)fcvt_to_i32(src, regs));
					std::fesetround(old_round);
					regs.or_fflags(collect_fflags());
					write_velem(regs, instr.rd, wsew, i, xr & elem_mask(wsew));
					break;
				}
				case 0x0A: case 0x0B: { // float <- int, widened
					std::fesetround(rm_default);
					uint64_t raw = read_velem(regs, instr.rs2, nsew, i);
					double r0 = (sub == 0x0A) ? (double)raw : (double)sext_elem(raw, nsew);
					volatile double r = (wsew == 32) ? (double)(float)r0 : r0;
					std::fesetround(old_round);
					regs.or_fflags(collect_fflags());
					write_f(instr.rd, wsew, i, r, rm_default);
					break;
				}
				case 0x0C: { // vfwcvt.f.f.v -- the Zvfhmin one. Always exact:
					// every value of the narrow format is representable in
					// the wide one, so no rounding mode is consulted.
					double v = read_f(instr.rs2, nsew, i);
					write_f(instr.rd, wsew, i, v, rm_default);
					break;
				}
				default: break; // 0x0D is reserved
				}
			} else {
				switch (sub) {
				case 0x10: case 0x11: case 0x16: case 0x17: { // int <- float, narrowed
					std::fesetround((sub >= 0x16) ? FE_TOWARDZERO : rm_default);
					double src = read_f(instr.rs2, wsew, i);
					bool uns = (sub == 0x10 || sub == 0x16);
					volatile uint64_t xr = uns ? (uint64_t)fcvt_to_u32(src, regs)
					                           : (uint64_t)(int64_t)fcvt_to_i32(src, regs);
					std::fesetround(old_round);
					regs.or_fflags(collect_fflags());
					write_velem(regs, instr.rd, nsew, i, xr & elem_mask(nsew));
					break;
				}
				case 0x12: case 0x13: { // float <- int, narrowed
					std::fesetround(rm_default);
					uint64_t raw = read_velem(regs, instr.rs2, wsew, i);
					double r0 = (sub == 0x12) ? (double)raw : (double)sext_elem(raw, wsew);
					volatile double r = (nsew == 32) ? (double)(float)r0 : r0;
					std::fesetround(old_round);
					regs.or_fflags(collect_fflags());
					write_f(instr.rd, nsew, i, r, rm_default);
					break;
				}
				case 0x14: case 0x15: { // vfncvt.f.f.w, and .rod
					// 0x15 is round-to-odd, which exists so that a
					// narrowing done in two steps cannot double-round: it
					// forces the intermediate's low bit set whenever the
					// result is inexact.
					int rm = (sub == 0x15) ? FE_TOWARDZERO : rm_default;
					double v = read_f(instr.rs2, wsew, i);
					if (nsew == 16) {
						uint8_t fl = 0;
						uint16_t h = fp16::f64_bits_to_h(bits_from_f64(v), rm, fl);
						if (sub == 0x15 && (fl & fp16::FLAG_NX)) h |= 1; // round to odd
						regs.or_fflags(fl);
						write_velem(regs, instr.rd, 16, i, h);
					} else {
						std::fesetround(rm);
						volatile double r = (double)(float)v;
						std::fesetround(old_round);
						uint8_t fl = collect_fflags();
						uint32_t fb = bits_from_f32((float)r);
						if (sub == 0x15 && (fl & 0x01)) fb |= 1;
						regs.or_fflags(fl);
						write_velem(regs, instr.rd, 32, i, fb);
					}
					break;
				}
				default: break;
				}
			}
			std::fesetround(old_round);
		});
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
		if (funct6 >= 0x30 && funct6 <= 0x3f) { // widening add/sub/mul/FMA and vfwredusum/vfwredosum
			// Widening reads narrow elements and computes at double the
			// width. Both 16->32 and 32->64 are real: this used to accept
			// only the latter, which made every e16 widening op a no-op and
			// accounts for about a third of what riscv-vector-tests was
			// failing.
			//
			// The arithmetic happens at the *wide* width, which is the whole
			// point of a widening operation -- one rounding, at the wider
			// format, rather than computing narrow and converting.
			const int wsew = sew * 2;
			if ((sew != 16 && sew != 32) || wsew > 64) return;

			bool op2_is_wide = (funct6 == 0x34 || funct6 == 0x36);
			auto read_vs2w = [&](uint64_t i) -> double {
				return read_felem(regs, instr.rs2, op2_is_wide ? wsew : sew, i);
			};
			// The .vf scalar comes from an f register at the narrow width,
			// widened on the way in like a vector element would be.
			auto narrow2 = [&](uint64_t i) -> double {
				if (is_vv) return read_felem(regs, instr.rs1, sew, i);
				if (sew == 16) {
					uint8_t f = 0;
					return f64_from_bits(fp16::h_to_f64_bits(
						fp16::unbox_f16(bits_from_f64(regs.read_f(instr.rs1))), f));
				}
				return (double)read_f32_reg(regs, instr.rs1);
			};
			if (funct6 == 0x31 || funct6 == 0x33) { // vfwredusum.vs / vfwredosum.vs (OPFVV only)
				double acc = read_felem(regs, instr.rs1, wsew, 0);
				for_each_active(regs, vm, vl, [&](uint64_t i) {
					acc = gbinop(acc, read_felem(regs, instr.rs2, sew, i), '+', rm, wsew, regs);
				});
				if (vl > 0) write_felem(regs, instr.rd, wsew, 0, acc);
				return;
			}
			switch (funct6) {
			case 0x30: case 0x34: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, wsew, i, gbinop(read_vs2w(i), narrow2(i), '+', rm, wsew, regs)); }); break;
			case 0x32: case 0x36: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, wsew, i, gbinop(read_vs2w(i), narrow2(i), '-', rm, wsew, regs)); }); break;
			case 0x38: for_each_active(regs, vm, vl, [&](uint64_t i) { write_felem(regs, instr.rd, wsew, i, gbinop(read_vs2w(i), narrow2(i), '*', rm, wsew, regs)); }); break;
			case 0x3c: case 0x3d: case 0x3e: case 0x3f: { // vfwmacc/vfwnmacc/vfwmsac/vfwnmsac (macc-family only, no widening madd-family)
				uint8_t neg = funct6 & 0x3;
				bool negate_a = (neg == 0b01 || neg == 0b11);
				bool negate_c = (neg == 0b01 || neg == 0b10);
				for_each_active(regs, vm, vl, [&](uint64_t i) {
					double a = narrow2(i), b = read_vs2w(i);
					double c = read_felem(regs, instr.rd, wsew, i);
					if (negate_a) a = -a;
					if (negate_c) c = -c;
					write_felem(regs, instr.rd, wsew, i, gfma(a, b, c, rm, wsew, regs));
				});
				break;
			}
			}
		}
		break;
	}
}

} // namespace vcommon
