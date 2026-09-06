// IEEE 754 binary16 conversion, done in integer arithmetic.
//
// Shared by Zfhmin (scalar fcvt.s.h / fcvt.h.s / fcvt.d.h / fcvt.h.d) and
// Zvfhmin (vector vfwcvt.f.f.v / vfncvt.f.f.w), which is why it lives in a
// header rather than in either file.
//
// The host FPU is not used. MinGW's GCC 8.1 has no _Float16 on x86, so there
// is no 16-bit type to round into and no way to let the hardware do it.
//
// More importantly, the obvious workaround is wrong: converting a double to
// a float and then to a half rounds twice, and a value that sits exactly
// halfway between two halves can be nudged off that midpoint by the first
// step and then round the wrong way in the second. So narrowing is always
// done in a single step from the source significand, with a sticky bit --
// narrow_to_h below is shared by the float and double paths for exactly
// that reason.
//
// Widening needs none of this: every binary16 value is exactly
// representable in binary32 and binary64, so it is bit shuffling that can
// raise nothing except invalid, from a signalling NaN.
#pragma once
#include <cfenv>
#include <cstdint>

namespace fp16 {

// Exception flag bits, matching fcsr's fflags layout.
constexpr uint8_t FLAG_NX = 0x01; // inexact
constexpr uint8_t FLAG_UF = 0x02; // underflow
constexpr uint8_t FLAG_OF = 0x04; // overflow
constexpr uint8_t FLAG_NV = 0x10; // invalid

constexpr uint16_t H_CANONICAL_NAN = 0x7E00;

inline bool h_is_nan(uint16_t h)  { return (h & 0x7C00) == 0x7C00 && (h & 0x03FF); }
inline bool h_is_snan(uint16_t h) { return h_is_nan(h) && !(h & 0x0200); }

// A half held in an f register is NaN-boxed like a single, but with 48 bits
// of ones above it rather than 32.
inline uint64_t box_f16(uint16_t h) { return 0xFFFFFFFFFFFF0000ull | (uint64_t)h; }

// Anything not properly boxed reads back as the canonical NaN -- the same
// rule single precision already follows.
inline uint16_t unbox_f16(uint64_t boxed)
{
	if ((boxed & 0xFFFFFFFFFFFF0000ull) != 0xFFFFFFFFFFFF0000ull) return H_CANONICAL_NAN;
	return (uint16_t)boxed;
}

//---------------------------------------------------------------------
// Widening: half -> single / double. Always exact.
//---------------------------------------------------------------------

// Decompose a half into a sign, an unbiased exponent and a significand with
// the leading one made explicit, renormalising a subnormal in the process.
// Returns false for inf/NaN, which the callers handle separately.
inline bool h_decompose(uint16_t h, uint32_t &sign, int32_t &e, uint32_t &man)
{
	sign = (uint32_t)((h >> 15) & 1);
	uint32_t exp = (h >> 10) & 0x1F;
	man = h & 0x03FF;
	if (exp == 0x1F) return false;
	if (exp == 0) {
		if (man == 0) { e = 0; return true; } // zero: man == 0 marks it
		int shift = 0;
		while (!(man & 0x0400)) { man <<= 1; shift++; }
		man &= 0x03FF;
		e = -14 - shift;
		return true;
	}
	e = (int32_t)exp - 15;
	return true;
}

inline uint32_t h_to_f32_bits(uint16_t h, uint8_t &flags)
{
	uint32_t sign, man; int32_t e;
	if (!h_decompose(h, sign, e, man)) {
		uint32_t s = sign << 31;
		if ((h & 0x03FF) == 0) return s | 0x7F800000u; // infinity
		if (h_is_snan(h)) flags |= FLAG_NV;
		return 0x7FC00000u; // RISC-V hands back the canonical NaN
	}
	uint32_t s = sign << 31;
	if (man == 0 && e == 0 && (h & 0x7FFF) == 0) return s; // signed zero
	return s | ((uint32_t)(e + 127) << 23) | (man << 13);
}

inline uint64_t h_to_f64_bits(uint16_t h, uint8_t &flags)
{
	uint32_t sign, man; int32_t e;
	if (!h_decompose(h, sign, e, man)) {
		uint64_t s = (uint64_t)sign << 63;
		if ((h & 0x03FF) == 0) return s | 0x7FF0000000000000ull;
		if (h_is_snan(h)) flags |= FLAG_NV;
		return 0x7FF8000000000000ull;
	}
	uint64_t s = (uint64_t)sign << 63;
	if (man == 0 && e == 0 && (h & 0x7FFF) == 0) return s;
	return s | ((uint64_t)(e + 1023) << 52) | ((uint64_t)man << 42);
}

//---------------------------------------------------------------------
// Narrowing: single / double -> half. Rounds exactly once.
//---------------------------------------------------------------------

// The shared core. `sig` carries the source significand with its leading one
// explicit, `frac_bits` says how many fraction bits sit below that one, and
// `e` is the unbiased exponent. Every case that is easy to get wrong lives
// here: values landing in half's subnormal range, exact midpoints, rounding
// that carries a subnormal up into the normal range, and overflow -- which
// does not always mean infinity, since rounding toward zero produces the
// largest finite magnitude instead.
inline uint16_t narrow_to_h(uint32_t sign, int32_t e, uint64_t sig,
                            int frac_bits, int round_mode, uint8_t &flags)
{
	uint16_t s16 = (uint16_t)(sign << 15);

	// Line the result's units bit up so `shift` bits are discarded below it.
	int shift = frac_bits - 10;
	if (e < -14) {
		shift += (-14 - e);
		e = -14;
		if (shift > 63) {
			// Everything shifted away, but it still has to round: a tiny
			// value rounds up to the smallest subnormal when the mode
			// pushes away from zero.
			flags |= FLAG_NX | FLAG_UF;
			bool up = (round_mode == FE_UPWARD && !sign)
			       || (round_mode == FE_DOWNWARD && sign);
			return (uint16_t)(s16 | (up ? 1 : 0));
		}
	}

	uint64_t lsb = 1ull << shift;
	uint64_t rem = sig & (lsb - 1);
	uint64_t out = sig >> shift;

	bool inexact = rem != 0;
	if (inexact) {
		uint64_t half = lsb >> 1;
		bool up = false;
		switch (round_mode) {
		case FE_TOWARDZERO: up = false; break;
		case FE_UPWARD:     up = !sign; break;
		case FE_DOWNWARD:   up = sign;  break;
		default:            up = (rem > half) || (rem == half && (out & 1)); break;
		}
		if (up) out++;
	}

	int32_t biased = e + 15;
	if (e == -14 && !(out & 0x0400)) {
		// Still subnormal. Underflow is signalled only when the result is
		// also inexact, which is the tininess-after-rounding rule.
		if (inexact) flags |= FLAG_UF | FLAG_NX;
		return (uint16_t)(s16 | (uint16_t)(out & 0x03FF));
	}
	if (out & 0x0800) { out >>= 1; biased++; }  // rounding carried out the top
	else if (e == -14) biased = 1;              // a subnormal rounded up into normal

	if (biased >= 0x1F) {
		flags |= FLAG_OF | FLAG_NX;
		bool to_inf = (round_mode == FE_TONEAREST)
		           || (round_mode == FE_UPWARD && !sign)
		           || (round_mode == FE_DOWNWARD && sign);
		return (uint16_t)(s16 | (to_inf ? 0x7C00 : 0x7BFF));
	}
	if (inexact) flags |= FLAG_NX;
	return (uint16_t)(s16 | ((uint16_t)biased << 10) | (uint16_t)(out & 0x03FF));
}

inline uint16_t f32_bits_to_h(uint32_t f, int round_mode, uint8_t &flags)
{
	uint32_t sign = (f >> 31) & 1;
	int32_t exp = (int32_t)((f >> 23) & 0xFF);
	uint32_t man = f & 0x007FFFFF;

	if (exp == 0xFF) {
		if (man == 0) return (uint16_t)((sign << 15) | 0x7C00);
		if (!(man & 0x00400000u)) flags |= FLAG_NV; // signalling NaN
		return H_CANONICAL_NAN;
	}
	if (exp == 0 && man == 0) return (uint16_t)(sign << 15);

	int32_t e = exp ? exp - 127 : -126;
	uint64_t sig = man | (exp ? 0x00800000u : 0u);
	return narrow_to_h(sign, e, sig, 23, round_mode, flags);
}

inline uint16_t f64_bits_to_h(uint64_t d, int round_mode, uint8_t &flags)
{
	uint32_t sign = (uint32_t)((d >> 63) & 1);
	int32_t exp = (int32_t)((d >> 52) & 0x7FF);
	uint64_t man = d & 0x000FFFFFFFFFFFFFull;

	if (exp == 0x7FF) {
		if (man == 0) return (uint16_t)((sign << 15) | 0x7C00);
		if (!(man & 0x0008000000000000ull)) flags |= FLAG_NV;
		return H_CANONICAL_NAN;
	}
	if (exp == 0 && man == 0) return (uint16_t)(sign << 15);

	int32_t e = exp ? exp - 1023 : -1022;
	uint64_t sig = man | (exp ? 0x0010000000000000ull : 0ull);
	// Straight from the double's 52 fraction bits, not via float: two
	// roundings can disagree with one at an exact half-precision midpoint.
	return narrow_to_h(sign, e, sig, 52, round_mode, flags);
}

} // namespace fp16
