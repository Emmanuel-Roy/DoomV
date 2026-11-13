#pragma once
// DoomV's floating-point unit: Berkeley SoftFloat and bit patterns, nothing else.
//
// Every floating-point value in the machine is a bit pattern -- in the f
// registers, in vector elements, in memory -- and the only thing that ever
// interprets one is SoftFloat, through the functions below. There is no host
// float or double anywhere on the way: no host arithmetic, no host rounding
// mode, no host exception flags, and no host conversion between widths.
//
// It was not always so. The F/D arithmetic moved to SoftFloat first, when
// riscv-arch-test showed the host's flags to be unreliable (MXCSR under an
// x86-on-ARM translator, a libm fma that never touched it) and its rounding
// modes to be one short (x86 has no ties-away, RISC-V's RMM). Everything
// around the arithmetic stayed host-shaped, and each piece left behind was a
// bug of its own: conversions that rounded RMM ties to even, fmin/fmax that
// returned whatever glibc's fmin does with a signaling NaN, and vector
// elements narrowed through a host cast that quietly quieted signaling NaNs
// (BUGS 193-195). So the unit is built the other way round now: SoftFloat
// types and bit patterns from register to register, and a host float type
// is not something this code can name.
//
// The pieces:
//   * Fmt<T>: the three formats -- half, single, double -- as SoftFloat's own
//     types float16_t / float32_t / float64_t, with their bit layout.
//   * Overloads of every SoftFloat operation by type, so one template serves
//     all three formats: add(a, b) is f16_add, f32_add or f64_add.
//   * The few operations SoftFloat does not have as such -- sign injection,
//     min/max, the comparison an instruction names -- written on the bits.
//   * Register access, with NaN-boxing.
//
// Flags: every operation reports through softfloat_exceptionFlags, including
// the hand-written ones here (fpu::raise). The caller brackets the work with
// sf::begin(rm, frm) / sf::end(regs), which installs the rounding mode,
// clears the flags, and folds them into fflags afterwards.
#include <cstdint>
#include "ext_softfloat.hpp"
#include "registers.hpp"
#include "riscv_decoder.hpp"

namespace fpu {

// ---- formats ------------------------------------------------------------------

template <class T> struct Fmt;
template <> struct Fmt<float16_t> {
	using Bits = uint16_t;
	static constexpr int width = 16;
	static constexpr Bits sign = 0x8000, exp = 0x7C00, quiet = 0x0200, nan = 0x7E00;
};
template <> struct Fmt<float32_t> {
	using Bits = uint32_t;
	static constexpr int width = 32;
	static constexpr Bits sign = 0x80000000u, exp = 0x7F800000u, quiet = 0x00400000u, nan = 0x7FC00000u;
};
template <> struct Fmt<float64_t> {
	using Bits = uint64_t;
	static constexpr int width = 64;
	static constexpr Bits sign = 0x8000000000000000ull, exp = 0x7FF0000000000000ull,
	                      quiet = 0x0008000000000000ull, nan = 0x7FF8000000000000ull;
};

// The next format up, for the widening vector instructions.
template <class T> struct Wider;
template <> struct Wider<float16_t> { using type = float32_t; };
template <> struct Wider<float32_t> { using type = float64_t; };

template <class T> inline typename Fmt<T>::Bits bits(T a) { return (typename Fmt<T>::Bits)a.v; }
template <class T> inline T make(uint64_t b) { T a; a.v = (typename Fmt<T>::Bits)b; return a; }

template <class T> inline bool is_nan(T a)
{
	// Exponent all ones and a nonzero significand: past infinity, ignoring sign.
	return (typename Fmt<T>::Bits)(bits(a) & (typename Fmt<T>::Bits)~Fmt<T>::sign) > Fmt<T>::exp;
}
template <class T> inline bool is_snan(T a) { return is_nan(a) && !(bits(a) & Fmt<T>::quiet); }
template <class T> inline bool is_zero(T a) { return (typename Fmt<T>::Bits)(bits(a) << 1) == 0; }
template <class T> inline bool is_negative(T a) { return (bits(a) & Fmt<T>::sign) != 0; }
template <class T> inline T negate(T a) { return make<T>(bits(a) ^ Fmt<T>::sign); }
template <class T> inline T canonical_nan() { return make<T>(Fmt<T>::nan); }

inline void raise(uint_fast8_t flags) { softfloat_exceptionFlags |= flags; }

// ---- SoftFloat, overloaded by format -------------------------------------------

#define FPU_SOFTFLOAT(T, p)                                                                       \
	inline T add(T a, T b) { return p##_add(a, b); }                                              \
	inline T sub(T a, T b) { return p##_sub(a, b); }                                              \
	inline T mul(T a, T b) { return p##_mul(a, b); }                                              \
	inline T div(T a, T b) { return p##_div(a, b); }                                              \
	inline T sqrt(T a) { return p##_sqrt(a); }                                                    \
	inline T mul_add(T a, T b, T c) { return p##_mulAdd(a, b, c); }                               \
	inline bool eq(T a, T b) { return p##_eq(a, b); }                                             \
	inline bool lt(T a, T b) { return p##_lt(a, b); }                                             \
	inline bool le(T a, T b) { return p##_le(a, b); }                                             \
	inline bool lt_quiet(T a, T b) { return p##_lt_quiet(a, b); }                                 \
	inline bool le_quiet(T a, T b) { return p##_le_quiet(a, b); }                                 \
	inline uint64_t classify(T a) { return p##_classify(a); }                                     \
	inline T min(T a, T b) { return p##_min(a, b); }                                              \
	inline T max(T a, T b) { return p##_max(a, b); }                                              \
	inline T round_to_int(T a, uint_fast8_t rm, bool exact) { return p##_roundToInt(a, rm, exact); } \
	inline T rsqrte7(T a) { return p##_rsqrte7(a); }                                              \
	inline T recip7(T a) { return p##_recip7(a); }                                                \
	inline int32_t to_i32(T a, uint_fast8_t rm) { return (int32_t)p##_to_i32(a, rm, true); }      \
	inline uint32_t to_ui32(T a, uint_fast8_t rm) { return (uint32_t)p##_to_ui32(a, rm, true); }  \
	inline int64_t to_i64(T a, uint_fast8_t rm) { return (int64_t)p##_to_i64(a, rm, true); }      \
	inline uint64_t to_ui64(T a, uint_fast8_t rm) { return (uint64_t)p##_to_ui64(a, rm, true); }
FPU_SOFTFLOAT(float16_t, f16)
FPU_SOFTFLOAT(float32_t, f32)
FPU_SOFTFLOAT(float64_t, f64)
#undef FPU_SOFTFLOAT

// To integers narrower than 32 bits, for the vector conversions: SoftFloat's
// own, which saturate to the destination and raise invalid alone, as a
// conversion out of range does.
inline int16_t to_i16(float16_t a, uint_fast8_t rm) { return (int16_t)f16_to_i16(a, rm, true); }
inline int16_t to_i16(float32_t a, uint_fast8_t rm) { return (int16_t)f32_to_i16(a, rm, true); }
inline uint16_t to_ui16(float16_t a, uint_fast8_t rm) { return (uint16_t)f16_to_ui16(a, rm, true); }
inline uint16_t to_ui16(float32_t a, uint_fast8_t rm) { return (uint16_t)f32_to_ui16(a, rm, true); }
inline int8_t to_i8(float16_t a, uint_fast8_t rm) { return (int8_t)f16_to_i8(a, rm, true); }
inline uint8_t to_ui8(float16_t a, uint_fast8_t rm) { return (uint8_t)f16_to_ui8(a, rm, true); }

// Integer to float, rounded once, at the destination's own width.
template <class T> T from_i32(int32_t);
template <class T> T from_ui32(uint32_t);
template <class T> T from_i64(int64_t);
template <class T> T from_ui64(uint64_t);
#define FPU_FROM_INT(T, p)                                                      \
	template <> inline T from_i32<T>(int32_t x) { return i32_to_##p(x); }       \
	template <> inline T from_ui32<T>(uint32_t x) { return ui32_to_##p(x); }    \
	template <> inline T from_i64<T>(int64_t x) { return i64_to_##p(x); }       \
	template <> inline T from_ui64<T>(uint64_t x) { return ui64_to_##p(x); }
FPU_FROM_INT(float16_t, f16)
FPU_FROM_INT(float32_t, f32)
FPU_FROM_INT(float64_t, f64)
#undef FPU_FROM_INT

// Between formats. Widening is exact; narrowing rounds once, from the source
// itself. A NaN becomes the destination's canonical NaN and a signaling one
// raises invalid, in either direction.
template <class To, class From> To convert(From a);
template <> inline float16_t convert<float16_t>(float16_t a) { return a; }
template <> inline float32_t convert<float32_t>(float16_t a) { return f16_to_f32(a); }
template <> inline float64_t convert<float64_t>(float16_t a) { return f16_to_f64(a); }
template <> inline float16_t convert<float16_t>(float32_t a) { return f32_to_f16(a); }
template <> inline float32_t convert<float32_t>(float32_t a) { return a; }
template <> inline float64_t convert<float64_t>(float32_t a) { return f32_to_f64(a); }
template <> inline float16_t convert<float16_t>(float64_t a) { return f64_to_f16(a); }
template <> inline float32_t convert<float32_t>(float64_t a) { return f64_to_f32(a); }
template <> inline float64_t convert<float64_t>(float64_t a) { return a; }

// ---- the operations SoftFloat has no single function for -----------------------

// FSGNJ / FSGNJN / FSGNJX (funct3 0, 1, 2): rs1's magnitude with a sign
// taken from rs2. Bit manipulation -- no rounding, no flags, and a NaN passes
// through with its payload, signaling or not.
template <class T> inline T sign_inject(T a, T b, unsigned funct3)
{
	using B = typename Fmt<T>::Bits;
	const B s = Fmt<T>::sign, x = bits(a), y = bits(b);
	const B sign = funct3 == 0 ? (B)(y & s) : funct3 == 1 ? (B)(~y & s) : (B)((x ^ y) & s);
	return make<T>((B)((x & (B)~s) | sign));
}

// FMIN / FMAX: IEEE 754-2019 minimumNumber / maximumNumber, which is what
// spike's SoftFloat f16/f32/f64_min and _max are. A NaN operand is skipped,
// two give the canonical NaN, a signaling one raises invalid even when the
// other operand is the answer, and -0 orders below +0. None of this is left
// to a C library: glibc's fmin returns a signaling NaN quieted where RISC-V
// returns the other operand (BUGS 194).
template <class T> inline T min_max(T a, T b, bool is_max) { return is_max ? max(a, b) : min(a, b); }

// Zfa's FMINM / FMAXM: minimum / maximum, where any NaN operand makes the
// result the canonical NaN instead of being skipped. SoftFloat's quiet
// equality raises the invalid a signaling operand calls for.
template <class T> inline T min_max_nan(T a, T b, bool is_max)
{
	if (is_nan(a) || is_nan(b)) {
		(void)eq(a, b);
		return canonical_nan<T>();
	}
	return min_max(a, b, is_max);
}

// The comparison an instruction's funct3 names. FEQ is quiet (only a
// signaling NaN raises invalid); FLT and FLE signal on any NaN; Zfa's FLEQ
// and FLTQ are the quiet forms of those two. SoftFloat has each one, so the
// flags come with the answer.
template <class T> inline bool compare(T a, T b, unsigned funct3)
{
	switch (funct3) {
	case 0:  return le(a, b);         // FLE
	case 1:  return lt(a, b);         // FLT
	case 2:  return eq(a, b);         // FEQ
	case 4:  return le_quiet(a, b);   // FLEQ (Zfa)
	default: return lt_quiet(a, b);   // FLTQ (Zfa)
	}
}

// ---- registers --------------------------------------------------------------------

// A value narrower than the register is NaN-boxed: every bit above it set.
inline uint64_t box(uint64_t value, int width)
{
	return width >= 64 ? value : (~0ull << width) | (value & ((1ull << width) - 1));
}

// An instruction that interprets a register as a number reads it unboxed: a
// value that is not properly boxed reads as the format's canonical NaN.
// (Moves and stores take the raw bits instead, and do not come through here.)
template <class T> inline T read(const Registers &r, int i)
{
	const uint64_t v = r.read_f_bits(i);
	if constexpr (Fmt<T>::width == 64) {
		return make<T>(v);
	} else {
		constexpr uint64_t top = ~0ull << Fmt<T>::width;
		return (v & top) == top ? make<T>(v) : canonical_nan<T>();
	}
}
template <class T> inline void write(Registers &r, int i, T a) { r.write_f_bits(i, box(bits(a), Fmt<T>::width)); }

// Calls fn with a value of the SoftFloat type for an element width.
template <class Fn> inline void for_width(int width, Fn &&fn)
{
	if (width == 16) fn(float16_t{});
	else if (width == 32) fn(float32_t{});
	else fn(float64_t{});
}

// ---- the scalar unit ----------------------------------------------------------------

// Every OP-FP and fused multiply-add instruction of F, D, Zfh, Zfhmin and Zfa
// -- everything but the loads and stores. The format is funct7's low two
// bits (00 single, 01 double, 10 half; for the fused forms, the same two
// bits are funct2), so one implementation serves all three; the decoder has
// already decided which encodings this machine accepts. Writes rd and the
// flags; the caller advances the pc.
void execute(const DecodedOp &instr, Registers &regs);

} // namespace fpu
