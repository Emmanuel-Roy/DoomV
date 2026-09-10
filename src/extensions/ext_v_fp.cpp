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
// Half-precision elements travel through `double`, and the conversion has to
// be lossless in *both* directions -- which the ordinary f16<->f64 helpers
// are not. h_to_f64_bits maps every NaN to the canonical double NaN,
// discarding the f16 payload and the sign, so a round trip turned any NaN
// into 0x7E00. That is wrong in three places at once:
//
//   * an element *move* (the slides, merge, vfmv) must copy bit patterns
//     verbatim, payload included -- it is a copy, not an operation;
//   * fsgnj and fclass are pure bit manipulation, and a classify that
//     cannot see the payload cannot tell a quiet NaN from a signalling one;
//   * arithmetic loses the invalid *flag*: the result would be canonicalised
//     by the architecture anyway, but an sNaN operand has to raise invalid,
//     and it had already been quieted before the operation saw it.
//
// So NaNs carry their exact 16-bit pattern in the low bits of a double NaN.
// Both directions are written here and nothing else produces these values,
// so the encoding only has to be self-consistent: exponent all ones and a
// nonzero mantissa make it a genuine double NaN, and the low sixteen bits
// hold the half unchanged. Finite values and infinities go through the
// ordinary conversion, which is already exact for them.
inline bool h_is_nan_bits(uint16_t h) { return (h & 0x7C00) == 0x7C00 && (h & 0x03FF); }

inline double h_to_d(uint16_t h)
{
	if (h_is_nan_bits(h)) return f64_from_bits(0x7FF8000000000000ull | (uint64_t)h);
	uint8_t f = 0;
	return f64_from_bits(fp16::h_to_f64_bits(h, f));
}

inline uint16_t d_to_h(double v)
{
	const uint64_t b = bits_from_f64(v);
	if ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull
	    && (b & 0x000FFFFFFFFFFFFFull)) {
		const uint16_t stored = (uint16_t)b;
		// A double NaN that carries one of ours gives the half back
		// exactly; any other double NaN -- one that arrived from a wider
		// format -- becomes the canonical half NaN, which is what the
		// architecture asks for.
		return h_is_nan_bits(stored) ? stored : 0x7E00;
	}
	uint8_t f = 0;
	return fp16::f64_bits_to_h(b, 0, f);
}


// vfrsqrt7 and vfrec7: the reciprocal-square-root and reciprocal estimates.
//
// These were approximated here with exact reciprocals, on the reasoning that
// being more accurate than the seven bits the spec asks for cannot hurt. It
// does hurt: the instructions are defined by a *lookup table*, not by an
// accuracy bound, so software that uses them as the seed of a Newton
// iteration gets a different number of correct bits than the architecture
// promises, and a conformance test comparing against the reference sees a
// mismatch on almost every input. "At least as accurate as required" is not
// the same as "what the architecture says".
//
// Both tables and the surrounding exponent arithmetic are transcribed from
// the Sail model's vext_fp_utils_insts.sail rather than from memory, which
// is the same reason the differential harness exists at all -- 128 entries
// recalled approximately would be worse than useless.
// Both tables are indexed directly. The model writes table[127 - idx],
// which looks like a reversal but is not: Sail vectors default to
// *descending* index order, so its element 127 is the first literal in the
// list and its element 0 is the last. Transcribing the literals into an
// ascending C array and keeping the 127 - idx made every lookup read the
// table backwards -- vfrec7(2.0) came out 0.25 instead of 0.498, an
// exponent that happened to be right with a significand that was not.
constexpr uint8_t RSQRT7_TABLE[128] = {
	52, 51, 50, 48, 47, 46, 44, 43,
	42, 41, 40, 39, 38, 36, 35, 34,
	33, 32, 31, 30, 30, 29, 28, 27,
	26, 25, 24, 23, 23, 22, 21, 20,
	19, 19, 18, 17, 16, 16, 15, 14,
	14, 13, 12, 12, 11, 10, 10,  9,
	 9,  8,  7,  7,  6,  6,  5,  4,
	 4,  3,  3,  2,  2,  1,  1,  0,
	127, 125, 123, 121, 119, 118, 116, 114,
	113, 111, 109, 108, 106, 105, 103, 102,
	100, 99, 97, 96, 95, 93, 92, 91,
	90, 88, 87, 86, 85, 84, 83, 82,
	80, 79, 78, 77, 76, 75, 74, 73,
	72, 71, 70, 70, 69, 68, 67, 66,
	65, 64, 63, 63, 62, 61, 60, 59,
	59, 58, 57, 56, 56, 55, 54, 53,
};

constexpr uint8_t RECIP7_TABLE[128] = {
	127, 125, 123, 121, 119, 117, 116, 114,
	112, 110, 109, 107, 105, 104, 102, 100,
	99, 97, 96, 94, 93, 91, 90, 88,
	87, 85, 84, 83, 81, 80, 79, 77,
	76, 75, 74, 72, 71, 70, 69, 68,
	66, 65, 64, 63, 62, 61, 60, 59,
	58, 57, 56, 55, 54, 53, 52, 51,
	50, 49, 48, 47, 46, 45, 44, 43,
	42, 41, 40, 40, 39, 38, 37, 36,
	35, 35, 34, 33, 32, 31, 31, 30,
	29, 28, 28, 27, 26, 25, 25, 24,
	23, 23, 22, 21, 21, 20, 19, 19,
	18, 17, 17, 16, 15, 15, 14, 14,
	13, 12, 12, 11, 11, 10,  9,  9,
	 8,  8,  7,  7,  6,  5,  5,  4,
	 4,  3,  3,  2,  2,  1,  1,  0,
};

inline void fp_fmt(int sew, int &e, int &sg)
{
	if (sew == 16)      { e = 5;  sg = 10; }
	else if (sew == 32) { e = 8;  sg = 23; }
	else                { e = 11; sg = 52; }
}

// Leading zeros counted over exactly `width` bits, which is what the
// subnormal normalisation needs -- a 64-bit clz would count the padding.
inline int clz_n(uint64_t v, int width)
{
	int n = 0;
	for (int i = width - 1; i >= 0; i--) {
		if ((v >> i) & 1ull) break;
		n++;
	}
	return n;
}

// Element classes, on raw bits, so the estimates never go through the
// double path -- they are bit-pattern operations and have no business
// canonicalising a NaN on the way in.
enum EClass { EC_SNAN, EC_QNAN, EC_INF, EC_ZERO, EC_SUB, EC_NORM };

inline EClass eclassify(uint64_t v, int sew, bool &neg)
{
	int e, sg; fp_fmt(sew, e, sg);
	const uint64_t sig = v & ((1ull << sg) - 1);
	const uint64_t exp = (v >> sg) & ((1ull << e) - 1);
	neg = ((v >> (sg + e)) & 1ull) != 0;
	if (exp == ((1ull << e) - 1))
		return sig == 0 ? EC_INF : ((sig >> (sg - 1)) & 1ull ? EC_QNAN : EC_SNAN);
	if (exp == 0) return sig == 0 ? EC_ZERO : EC_SUB;
	return EC_NORM;
}

inline uint64_t canonical_nan_bits(int sew)
{
	int e, sg; fp_fmt(sew, e, sg);
	return (((1ull << e) - 1) << sg) | (1ull << (sg - 1));
}

inline uint64_t inf_bits(int sew, bool neg)
{
	int e, sg; fp_fmt(sew, e, sg);
	uint64_t r = ((1ull << e) - 1) << sg;
	if (neg) r |= 1ull << (sg + e);
	return r;
}

uint64_t rsqrt7_core(uint64_t v, int sew, bool sub)
{
	int e, sg; fp_fmt(sew, e, sg);
	const uint64_t sig = v & ((1ull << sg) - 1);
	const uint64_t exp = (v >> sg) & ((1ull << e) - 1);
	const uint64_t sign = (v >> (sg + e)) & 1ull;

	int64_t nexp; uint64_t nsig;
	if (sub) {
		const int nlz = clz_n(sig, sg);
		nexp = -(int64_t)nlz;
		nsig = (sig << (1 + nlz)) & ((1ull << sg) - 1);
	} else {
		nexp = (int64_t)exp;
		nsig = sig;
	}

	// The index is the exponent's low bit above the significand's top six --
	// odd and even exponents need different table halves, because halving
	// the exponent for a square root leaves a factor of two behind on one
	// of them.
	const unsigned idx = (unsigned)(((uint64_t)(nexp & 1) << 6)
	                     | ((nsig >> (sg - 6)) & 0x3F));
	const uint64_t out_sig = (uint64_t)RSQRT7_TABLE[idx] << (sg - 7);
	const int64_t bias = (1ll << (e - 1)) - 1;
	// Truncating division, which is what C++ does for signed operands and
	// what the model specifies.
	const int64_t out_exp = (3 * bias - 1 - nexp) / 2;
	return (sign << (sg + e)) | (((uint64_t)out_exp & ((1ull << e) - 1)) << sg) | out_sig;
}

// Returns true when the result had to be forced to infinity or to the
// largest finite magnitude -- the subnormal input whose reciprocal
// overflows. The caller turns that into NX|OF.
bool recip7_core(uint64_t v, int sew, bool sub, uint8_t rm, uint64_t &out)
{
	int e, sg; fp_fmt(sew, e, sg);
	const uint64_t sig = v & ((1ull << sg) - 1);
	const uint64_t exp = (v >> sg) & ((1ull << e) - 1);
	const uint64_t sign = (v >> (sg + e)) & 1ull;
	const int nlz = clz_n(sig, sg);

	int64_t nexp; uint64_t nsig;
	if (sub) {
		nexp = -(int64_t)nlz;
		nsig = (sig << (1 + nlz)) & ((1ull << sg) - 1);
	} else {
		nexp = (int64_t)exp;
		nsig = sig;
	}

	const unsigned idx = (unsigned)((nsig >> (sg - 7)) & 0x7F);
	const int64_t bias = (1ll << (e - 1)) - 1;
	const uint64_t emask = (1ull << e) - 1;
	const uint64_t mid_exp = (uint64_t)(2 * bias - 1 - nexp) & emask;
	const uint64_t mid_sig = (uint64_t)RECIP7_TABLE[idx] << (sg - 7);

	uint64_t out_exp, out_sig;
	if (mid_exp == 0) {
		// The reciprocal has landed one exponent below normal: shift the
		// significand down and put the implicit bit back explicitly.
		out_exp = 0;
		out_sig = (mid_sig >> 1) | (1ull << (sg - 1));
	} else if (mid_exp == emask) {
		out_exp = 0;
		out_sig = (mid_sig >> 2) | (1ull << (sg - 2));
	} else {
		out_exp = mid_exp;
		out_sig = mid_sig;
	}

	if (sub && nlz > 1) {
		// Too small to reciprocate into the format. Which way it goes is
		// the rounding mode's decision: toward zero, or away from the
		// sign, gives the largest finite magnitude; everything else gives
		// infinity.
		const bool to_max = (rm == 1) || (rm == 2 && sign == 0) || (rm == 3 && sign == 1);
		if (to_max)
			// The largest finite magnitude: every exponent bit set except
			// the lowest, and a full significand. `emask >> 1` is not that
			// -- it clears the *top* bit instead of the bottom one, which
			// for binary16 gives 0x3FFF where the answer is 0x7BFF. The
			// difference only shows in the three rounding modes that take
			// this branch at all, which is why five probes at
			// round-to-nearest missed it.
			out = (sign << (sg + e)) | ((emask & ~1ull) << sg) | ((1ull << sg) - 1);
		else
			out = (sign << (sg + e)) | (emask << sg);
		return true;
	}
	out = (sign << (sg + e)) | (out_exp << sg) | out_sig;
	return false;
}


// bf16, the brain-float format: one sign bit, eight exponent bits, seven of
// significand. Its whole point is that it is the *top half of an f32* -- the
// exponent range is identical, so widening is a shift and nothing else, and
// narrowing loses only significand bits. That is why it exists: a
// machine-learning kernel wants f32's dynamic range at half the memory
// bandwidth, and does not care about the precision it gives up.
//
// Zvfbfmin adds the two conversions, Zvfbfwma the widening multiply-
// accumulate. Both are vector-only -- there is no scalar bf16 arithmetic to
// share with.
inline uint32_t bf16_to_f32_bits(uint16_t b) { return (uint32_t)b << 16; }

// Narrowing needs a rounding decision, and unlike every other narrowing
// conversion it cannot overflow the exponent -- there is no exponent to
// overflow, both formats have eight bits. So this rounds the 32-bit pattern
// and shifts, and a carry out of the significand walks into the exponent by
// itself, which is exactly right.
inline uint16_t f32_bits_to_bf16(uint32_t f, uint8_t rm, Registers &regs)
{
	const uint32_t exp = (f >> 23) & 0xFF;
	const uint32_t sig = f & 0x7FFFFF;
	if (exp == 0xFF) {
		// A NaN narrows to the destination's canonical NaN; an infinity
		// stays an infinity. Neither is a rounding decision.
		if (sig) return 0x7FC0;
		return (uint16_t)((f >> 16) & 0xFF80);
	}

	const uint32_t rem = f & 0xFFFF;          // the bits being discarded
	const bool neg = (f >> 31) != 0;
	uint32_t out = f >> 16;
	if (rem) {
		regs.or_fflags(0x01);                 // inexact
		switch (rm) {
		case 1: break;                                            // RTZ
		case 2: if (neg) out += 1; break;                         // RDN
		case 3: if (!neg) out += 1; break;                        // RUP
		case 4: if (rem >= 0x8000) out += 1; break;               // RMM
		default:                                                  // RNE
			if (rem > 0x8000 || (rem == 0x8000 && (out & 1))) out += 1;
			break;
		}
	}
	return (uint16_t)out;
}

double read_felem(Registers &regs, int base, int sew, uint64_t i)
{
	if (sew == 64) return f64_from_bits(read_velem(regs, base, 64, i));
	if (sew == 16) return h_to_d((uint16_t)read_velem(regs, base, 16, i));
	return (double)f32_from_bits((uint32_t)read_velem(regs, base, 32, i));
}
void write_felem(Registers &regs, int base, int sew, uint64_t i, double v)
{
	if (sew == 64) write_velem(regs, base, 64, i, bits_from_f64(v));
	else if (sew == 16) write_velem(regs, base, 16, i, d_to_h(v));
	else write_velem(regs, base, 32, i, bits_from_f32((float)v));
}

// Integer <-> half conversions, at half. These have to be done at binary16
// rather than by going through double and narrowing: an integer above 2048
// is not exactly representable, so computing in double and rounding once at
// the end rounds twice. And the destination is sixteen bits wide, so the
// result saturates to *that* range -- not to 32 bits and then truncated,
// which is what reusing the 32-bit converters did. Truncating a saturated
// 32-bit value gives a number, and the wrong one, with no invalid flag.
inline uint64_t h_to_int16(uint16_t h, bool uns, bool rtz, Registers &regs)
{
	sf::begin(rtz ? 1 : 7, regs.get_frm());   // 1 = RTZ, 7 = dynamic
	const uint_fast8_t mode = sf::round_mode(rtz ? 1 : 7, regs.get_frm());
	uint64_t out;
	if (uns) {
		uint_fast32_t v = f16_to_ui32(sf::f16(h), mode, true);
		out = (v > 0xFFFFu) ? 0xFFFFull : (uint64_t)v;
		if (v > 0xFFFFu) softfloat_exceptionFlags |= softfloat_flag_invalid;
	} else {
		int_fast32_t v = f16_to_i32(sf::f16(h), mode, true);
		int32_t c = (v > 32767) ? 32767 : (v < -32768 ? -32768 : (int32_t)v);
		if (v != c) softfloat_exceptionFlags |= softfloat_flag_invalid;
		out = (uint64_t)(uint16_t)(int16_t)c;
	}
	sf::end(regs);
	return out;
}

inline uint16_t int16_to_h(uint64_t raw, bool uns, Registers &regs)
{
	sf::begin(7, regs.get_frm());
	float16_t r = uns ? ui32_to_f16((uint32_t)(uint16_t)raw)
	                  : i32_to_f16((int32_t)(int16_t)(uint16_t)raw);
	sf::end(regs);
	return sf::bits(r);
}

inline uint16_t as_h(double v)  { return d_to_h(v); }
inline double from_h(uint16_t h) { return h_to_d(h); }

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
		if (w == 16)
			return h_to_d(fp16::unbox_f16(bits_from_f64(regs.read_f(instr.rs1))));
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
				// NaN-boxed, the way every half-precision producer leaves an
				// f register.
				regs.write_f(instr.rd, f64_from_bits(fp16::box_f16(d_to_h(v))));
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
		double scalar = read_fscalar(sew);   // at the element width, not always f32
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

		// Zvfbfmin's pair. Handled ahead of the generic widening and
		// narrowing arms because bf16 is not a width the rest of this file
		// knows about -- it is the same sixteen bits as a half, holding a
		// completely different format.
		if (sub == 0x0D && sew == 16) {   // vfwcvtbf16.f.f.v
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				const uint16_t b = (uint16_t)read_velem(regs, instr.rs2, 16, i);
				// Widening is a shift and exact -- except for NaNs. A
				// signalling NaN raises invalid and, like every other
				// format conversion, the result is the destination's
				// canonical NaN rather than the operand's payload widened.
				uint32_t r;
				if ((b & 0x7F80) == 0x7F80 && (b & 0x007F)) {
					if (!(b & 0x0040)) regs.or_fflags(0x10);
					r = 0x7FC00000u;
				} else {
					r = bf16_to_f32_bits(b);
				}
				write_velem(regs, instr.rd, 32, i, r);
			});
			return;
		}
		if (sub == 0x1D && sew == 16) {   // vfncvtbf16.f.f.w
			const uint8_t crm = (uint8_t)sf::round_mode(rm, regs.get_frm());
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, 16, i,
				            f32_bits_to_bf16((uint32_t)read_velem(regs, instr.rs2, 32, i),
				                             crm, regs));
			});
			return;
		}

		bool widening = sub < 0x10;
		int nsew = sew;              // the narrow side
		int wsew = sew * 2;          // the wide side
		if (wsew > 64) return;       // no format wider than 64 bits exists

		// Reading and writing a float element of a width the host has no
		// type for: 16-bit goes through the software conversion, 32 and 64
		// use the host float and double directly.
		auto read_f = [&](int vreg, int esew, uint64_t i) -> double {
			// No flag is raised for merely *reading* an element -- the
			// operation raises what it raises. The old path set invalid
			// here for an sNaN operand and then lost the operand itself.
			return read_felem(regs, vreg, esew, i);
		};
		auto write_f = [&](int vreg, int esew, uint64_t i, double v, int rm_riscv) {
			if (esew != 16) { write_felem(regs, vreg, esew, i, v); return; }
			// A narrowing write does round, so this one keeps the real
			// conversion and its flags rather than the lossless encoding.
			const uint64_t b = bits_from_f64(v);
			if ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull
			    && (b & 0x000FFFFFFFFFFFFFull)) {
				write_velem(regs, vreg, 16, i, d_to_h(v));
				return;
			}
			// SoftFloat rather than ext_fp16.hpp's narrowing, for RMM --
			// see the note in the vfncvt.f.f.w arm below.
			sf::begin(rm_riscv, regs.get_frm());
			uint16_t h = sf::bits(f64_to_f16(sf::f64(b)));
			sf::end(regs);
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
				case 0x0A: case 0x0B:
					if (nsew == 16) {
						std::fesetround(old_round);
						const uint64_t raw = read_velem(regs, instr.rs2, 16, i);
						sf::begin(7, regs.get_frm());
						float32_t r = (sub == 0x0A) ? ui32_to_f32((uint32_t)(uint16_t)raw)
						                            : i32_to_f32((int32_t)(int16_t)(uint16_t)raw);
						sf::end(regs);
						write_velem(regs, instr.rd, 32, i, sf::bits(r));
						break;
					}
					{ // float <- int, widened
					std::fesetround(rm_default);
					uint64_t raw = read_velem(regs, instr.rs2, nsew, i);
					double r0 = (sub == 0x0A) ? (double)raw : (double)sext_elem(raw, nsew);
					volatile double r = (wsew == 32) ? (double)(float)r0 : r0;
					std::fesetround(old_round);
					regs.or_fflags(collect_fflags());
					write_f(instr.rd, wsew, i, r, rm);
					break;
				}
				case 0x0C: { // vfwcvt.f.f.v -- the Zvfhmin one. Always exact:
					// every value of the narrow format is representable in
					// the wide one, so no rounding mode is consulted.
					//
					// A NaN is the exception, and not because of rounding:
					// the result is the destination's canonical NaN rather
					// than the operand's payload widened, and a signalling
					// operand raises invalid. Both have to be decided from
					// the raw source bits -- by the time the value has been
					// through a double the host may have quieted it, and the
					// flag is gone.
					const uint64_t nraw = read_velem(regs, instr.rs2, nsew, i);
					bool nneg = false;
					const EClass nc = eclassify(nraw, nsew, nneg);
					if (nc == EC_SNAN || nc == EC_QNAN) {
						if (nc == EC_SNAN) regs.or_fflags(0x10);
						write_velem(regs, instr.rd, wsew, i, canonical_nan_bits(wsew));
						break;
					}
					double v = read_f(instr.rs2, nsew, i);
					write_f(instr.rd, wsew, i, v, rm);
					break;
				}
				default: break; // 0x0D is reserved
				}
			} else {
				switch (sub) {
				case 0x10: case 0x11: case 0x16: case 0x17:
					if (nsew == 16) {
						// The wide source is a single, the narrow result a
						// sixteen-bit integer: convert at single and
						// saturate to the destination, rather than
						// truncating a 32-bit result into it.
						std::fesetround(old_round);
						const uint32_t wide = (uint32_t)read_velem(regs, instr.rs2, 32, i);
						const bool uns = (sub == 0x10 || sub == 0x16);
						const bool rtz = (sub == 0x16 || sub == 0x17);
						sf::begin(rtz ? 1 : 7, regs.get_frm());
						const uint_fast8_t md = sf::round_mode(rtz ? 1 : 7, regs.get_frm());
						uint64_t outv;
						if (uns) {
							uint_fast32_t v = f32_to_ui32(sf::f32(wide), md, true);
							outv = (v > 0xFFFFu) ? 0xFFFFull : (uint64_t)v;
							if (v > 0xFFFFu) softfloat_exceptionFlags |= softfloat_flag_invalid;
						} else {
							int_fast32_t v = f32_to_i32(sf::f32(wide), md, true);
							int32_t c = (v > 32767) ? 32767 : (v < -32768 ? -32768 : (int32_t)v);
							if (v != c) softfloat_exceptionFlags |= softfloat_flag_invalid;
							outv = (uint64_t)(uint16_t)(int16_t)c;
						}
						sf::end(regs);
						write_velem(regs, instr.rd, 16, i, outv);
						break;
					}
					{ // int <- float, narrowed
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
				case 0x12: case 0x13:
					if (nsew == 16) {
						std::fesetround(old_round);
						const uint64_t raw = read_velem(regs, instr.rs2, 32, i);
						sf::begin(7, regs.get_frm());
						float16_t r = (sub == 0x12) ? ui32_to_f16((uint32_t)raw)
						                            : i32_to_f16((int32_t)(uint32_t)raw);
						sf::end(regs);
						write_velem(regs, instr.rd, 16, i, sf::bits(r));
						break;
					}
					{ // float <- int, narrowed
					std::fesetround(rm_default);
					uint64_t raw = read_velem(regs, instr.rs2, wsew, i);
					double r0 = (sub == 0x12) ? (double)raw : (double)sext_elem(raw, wsew);
					volatile double r = (nsew == 32) ? (double)(float)r0 : r0;
					std::fesetround(old_round);
					regs.or_fflags(collect_fflags());
					write_f(instr.rd, nsew, i, r, rm);
					break;
				}
				case 0x14: case 0x15: { // vfncvt.f.f.w, and .rod
					// 0x15 is round-to-odd, which exists so that a
					// narrowing done in two steps cannot double-round: it
					// forces the intermediate's low bit set whenever the
					// result is inexact.
					// Named apart from the function's `rm`, which holds the
					// *RISC-V* rounding mode. This one is a host FE_* value
					// for the double path below, and the two are not
					// interchangeable: FE_TOWARDZERO is 0xC00 on x86, so
					// handing it to a uint8_t parameter expecting a RISC-V
					// mode truncates to 0 -- round-to-nearest-even. That
					// shadowing is what made every directed-rounding
					// overflow in the half path return an infinity where the
					// largest finite magnitude was required.
					const int host_rm = (sub == 0x15) ? FE_TOWARDZERO : rm_default;
					// Same NaN rule as the widening direction, and for the
					// same reason it has to come off the raw bits.
					const uint64_t wraw = read_velem(regs, instr.rs2, wsew, i);
					bool wneg = false;
					const EClass wc = eclassify(wraw, wsew, wneg);
					if (wc == EC_SNAN || wc == EC_QNAN) {
						if (wc == EC_SNAN) regs.or_fflags(0x10);
						write_velem(regs, instr.rd, nsew, i, canonical_nan_bits(nsew));
						break;
					}
					if (nsew == 16) {
						// Through SoftFloat, not the hand-written narrowing
						// in ext_fp16.hpp. That one takes a *host* rounding
						// mode, and x86 has no encoding for RMM (round to
						// nearest, ties away from zero) -- host_round_mode
						// folds it to nearest-even, so every tie under
						// frm=4 rounded the wrong way. It is the same defect
						// that moved F and D onto SoftFloat wholesale; this
						// one conversion was the last caller left behind.
						//
						// The source is read at its own width rather than
						// as a double, so the conversion is a single
						// rounding no matter which pair of formats it is.
						sf::begin((sub == 0x15) ? 1 : rm, regs.get_frm());
						float16_t r16 = (wsew == 64)
							? f64_to_f16(sf::f64(wraw))
							: f32_to_f16(sf::f32((uint32_t)wraw));
						uint16_t h = sf::bits(r16);
						// Round-to-odd: round toward zero, then force the low
						// bit whenever anything was discarded. It exists so
						// that narrowing in two steps cannot double-round.
						if (sub == 0x15 && (softfloat_exceptionFlags & softfloat_flag_inexact))
							h |= 1;
						sf::end(regs);
						write_velem(regs, instr.rd, 16, i, h);
					} else {
						double v = read_f(instr.rs2, wsew, i);
						std::fesetround(host_rm);
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
			if (sew == 16) {
				// Half goes through SoftFloat rather than the host FPU
				// path below, both for the saturation width and because
				// the host path cannot express RMM at all.
				std::fesetround(old_round);
				if (to_int)
					write_velem(regs, instr.rd, 16, i,
					            h_to_int16(d_to_h(read_felem(regs, instr.rs2, 16, i)),
					                       is_unsigned, force_rtz, regs));
				else
					// The unsigned flag differs by direction: 0x00/0x06 are
					// the unsigned float-to-int forms, but going the other
					// way it is 0x02 that is unsigned. Reusing is_unsigned
					// here read every f.xu as signed.
					write_velem(regs, instr.rd, 16, i,
					            int16_to_h(read_velem(regs, instr.rs2, 16, i),
					                       sub == 0x02, regs));
			} else if (to_int) {
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
			case 0x04: { // vfrsqrt7.v
				const uint64_t raw = read_velem(regs, instr.rs2, sew, i);
				bool neg = false;
				uint64_t r;
				switch (eclassify(raw, sew, neg)) {
				case EC_SNAN: regs.or_fflags(0x10); r = canonical_nan_bits(sew); break;
				case EC_QNAN: r = canonical_nan_bits(sew); break;
				case EC_ZERO: regs.or_fflags(0x08); r = inf_bits(sew, neg); break;
				case EC_INF:
					// +inf gives +0; -inf is a negative operand and invalid.
					if (neg) { regs.or_fflags(0x10); r = canonical_nan_bits(sew); }
					else r = 0;
					break;
				default:
					// Any negative finite operand is invalid: there is no
					// real square root to take the reciprocal of.
					if (neg) { regs.or_fflags(0x10); r = canonical_nan_bits(sew); }
					else r = rsqrt7_core(raw, sew, eclassify(raw, sew, neg) == EC_SUB);
					break;
				}
				write_velem(regs, instr.rd, sew, i, r);
				break;
			}
			case 0x05: { // vfrec7.v
				const uint64_t raw = read_velem(regs, instr.rs2, sew, i);
				bool neg = false;
				const EClass c = eclassify(raw, sew, neg);
				uint64_t r;
				bool abnormal = false;
				switch (c) {
				case EC_SNAN: regs.or_fflags(0x10); r = canonical_nan_bits(sew); break;
				case EC_QNAN: r = canonical_nan_bits(sew); break;
				case EC_ZERO: regs.or_fflags(0x08); r = inf_bits(sew, neg); break;
				case EC_INF:  r = neg ? (1ull << (sew - 1)) : 0; break; // signed zero
				default:
					abnormal = recip7_core(raw, sew, c == EC_SUB,
					                       (uint8_t)sf::round_mode(rm, regs.get_frm()), r);
					break;
				}
				if (abnormal) regs.or_fflags(0x01 | 0x04); // NX | OF
				write_velem(regs, instr.rd, sew, i, r);
				break;
			}
			case 0x10: {
				// Classify on raw bits, for the same reason the estimates do.
				const uint64_t raw = read_velem(regs, instr.rs2, sew, i);
				bool neg = false;
				uint64_t cls;
				switch (eclassify(raw, sew, neg)) {
				case EC_INF:  cls = neg ? (1ull << 0) : (1ull << 7); break;
				case EC_SNAN: cls = 1ull << 8; break;
				case EC_QNAN: cls = 1ull << 9; break;
				case EC_ZERO: cls = neg ? (1ull << 3) : (1ull << 4); break;
				case EC_SUB:  cls = neg ? (1ull << 2) : (1ull << 5); break;
				default:      cls = neg ? (1ull << 1) : (1ull << 6); break;
				}
				write_velem(regs, instr.rd, sew, i, cls);
				break;
			}
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
				if (sew == 16)
					return h_to_d(fp16::unbox_f16(bits_from_f64(regs.read_f(instr.rs1))));
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
			if (funct6 == 0x3b && sew == 16) { // vfwmaccbf16.vv / .vf
				// Both multiplicands widen exactly -- a bf16 is the top half
				// of an f32 -- so the only rounding is the one the fused
				// multiply-add itself performs.
				const uint16_t fscalar = fp16::unbox_f16(bits_from_f64(regs.read_f(instr.rs1)));
				for_each_active(regs, vm, vl, [&](uint64_t i) {
					const uint32_t a = is_vv
						? bf16_to_f32_bits((uint16_t)read_velem(regs, instr.rs1, 16, i))
						: bf16_to_f32_bits(fscalar);
					const uint32_t b = bf16_to_f32_bits((uint16_t)read_velem(regs, instr.rs2, 16, i));
					const uint32_t c = (uint32_t)read_velem(regs, instr.rd, 32, i);
					sf::begin(rm, regs.get_frm());
					float32_t r = f32_mulAdd(sf::f32(a), sf::f32(b), sf::f32(c));
					sf::end(regs);
					write_velem(regs, instr.rd, 32, i, sf::bits(r));
				});
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
