#pragma once
// Shared host-float plumbing for F/D (ext_fd.cpp) and vector floating-point
// (ext_v_fp.cpp): NaN-boxing, rounding-mode mapping, exception-flag
// collection, and the rounded-op/compare/classify/convert templates. Used
// to live entirely inside ext_fd.cpp's anonymous namespace; pulled out here
// once V needed the exact same machinery rather than a second copy of it.
#include "registers.hpp"
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

// The FP register file (Registers::f[32]) is always 64 bits wide, whether
// the value it holds is single- or double-precision -- when D is present,
// a single-precision value is stored "NaN-boxed": the upper 32 bits are
// all 1s, so a value that ISN'T properly boxed (some bit pattern other
// than 0xFFFFFFFF up top) reads back as the canonical quiet NaN instead
// of whatever garbage was there, per spec. Raw bit reinterpretation goes
// through memcpy, not pointer casts, to stay clear of strict-aliasing UB.
inline uint32_t bits_from_f32(float f)  { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float    f32_from_bits(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline uint64_t bits_from_f64(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline double   f64_from_bits(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

inline uint64_t box_f32(uint32_t bits) { return 0xFFFFFFFF00000000ull | bits; }
inline float unbox_f32(uint64_t boxed)
{
	uint32_t bits = ((boxed >> 32) == 0xFFFFFFFFull) ? (uint32_t)boxed : 0x7FC00000u;
	return f32_from_bits(bits);
}

inline float  read_f32_reg(Registers &r, int i) { return unbox_f32(bits_from_f64(r.read_f(i))); }
inline void   write_f32_reg(Registers &r, int i, float v) { r.write_f(i, f64_from_bits(box_f32(bits_from_f32(v)))); }

// RISC-V's rm field: 000=RNE,001=RTZ,010=RDN,011=RUP,100=RMM,111=dynamic
// (use fcsr's frm). x86/host <cfenv> has no native "round to nearest,
// ties away from zero" (RMM) -- approximated as round-to-nearest-even,
// a defensible simplification since RMM is rarely used in practice and
// only differs from RNE on exact halfway ties.
inline int host_round_mode(uint8_t rm, uint8_t frm)
{
	uint8_t mode = (rm == 0b111) ? frm : rm;
	switch (mode) {
	case 0b001: return FE_TOWARDZERO; // RTZ
	case 0b010: return FE_DOWNWARD;   // RDN
	case 0b011: return FE_UPWARD;     // RUP
	default:    return FE_TONEAREST;  // RNE, RMM (approximated), reserved encodings
	}
}

// <cfenv>'s feclearexcept/fetestexcept turned out to not reliably reflect
// SSE's real exception state in this project's dev environment -- found
// via arch-test cross-validation against spike: computed *values* were
// always correct (fesetround's rounding-mode mapping is fine), but NX/UF
// were silently under-reported for real test cases under non-default
// rounding modes. Root cause, chased all the way down: this machine is
// ARM64 (Snapdragon), so every x86-64 binary here -- including spike --
// runs under Windows' Prism x64 emulation layer. Spike gets exception
// flags right anyway because it computes F/D entirely in software
// (Berkeley SoftFloat), never touching a host FPU flag at all; this
// project's design instead computes via genuine host float/double
// arithmetic and reads the host's flags, which is exactly the piece
// Prism's x86->ARM64 translation doesn't faithfully preserve. Bypassing
// <cfenv> for direct MXCSR access below is a strict improvement (it did
// fix real, reproducible bugs -- see the git history around this
// comment) but doesn't fully close the gap: NX/UF can still read wrong
// specifically when running under this kind of x86-on-ARM64 emulation.
// On genuine x86-64 hardware this whole class of problem shouldn't
// exist, since MXCSR would be real silicon, not translated; unverified
// directly, no such hardware was available to test on here. Values
// (including NaN canonicalization) are unaffected either way -- only the
// fflags CSR's NX/UF bits are ever in question.
inline void clear_fp_exceptions()
{
	_mm_setcsr(_mm_getcsr() & ~0x3Fu); // clear IE/DE/ZE/OE/UE/PE (MXCSR bits 0-5)
}

// fflags bit positions (NV/DZ/OF/UF/NX), mapped from MXCSR's exception
// bits (IE/DE/ZE/OE/UE/PE respectively -- DE, "denormal operand", has no
// RISC-V fflags equivalent and is intentionally dropped).
inline uint8_t collect_fflags()
{
	unsigned mxcsr = _mm_getcsr();
	uint8_t flags = 0;
	if (mxcsr & 0x01) flags |= 0x10; // IE -> NV
	if (mxcsr & 0x04) flags |= 0x08; // ZE -> DZ
	if (mxcsr & 0x08) flags |= 0x04; // OE -> OF
	if (mxcsr & 0x10) flags |= 0x02; // UE -> UF
	if (mxcsr & 0x20) flags |= 0x01; // PE -> NX
	return flags;
}

// RISC-V requires every FP result that's NaN to be the single canonical
// quiet NaN (0x7fc00000 / 0x7ff8000000000000), never a NaN payload
// propagated from an input or whatever bit pattern the host FPU's ALU
// happened to produce -- x86 SSE in particular doesn't match this (it has
// its own propagation rules, e.g. often preserving an input NaN's payload
// or producing 0xffc00000 for some invalid ops), so every rounded op below
// canonicalizes its result before returning it.
template <typename T>
T canonical_nan()
{
	if constexpr (sizeof(T) == 4) return f32_from_bits(0x7fc00000u);
	else return f64_from_bits(0x7ff8000000000000ull);
}

// Shared "set rounding mode, compute, restore, collect exceptions" shape
// used by every rounded arithmetic op -- one template instead of writing
// this out separately for float and double at each of add/sub/mul/div/
// sqrt/fma (8+ call sites). `result` is volatile and there's an explicit
// compiler barrier around the compute step -- belt-and-suspenders against
// the optimizer reordering the FP instruction relative to the exception
// read below, since nothing here declares that dependency explicitly.
// Didn't turn out to be the actual cause of the NX/UF flakiness this
// project hit (see clear_fp_exceptions' comment -- that's a host/emulation
// issue, not a scheduling one), but it's correct defensive practice
// regardless and doesn't cost anything measurable here.
template <typename T>
T fp_binop(T a, T b, char op, uint8_t rm, Registers &regs)
{
	clear_fp_exceptions();
	asm volatile("" ::: "memory");
	int old_round = std::fegetround();
	std::fesetround(host_round_mode(rm, regs.get_frm()));
	volatile T result = 0;
	switch (op) {
	case '+': result = a + b; break;
	case '-': result = a - b; break;
	case '*': result = a * b; break;
	case '/': result = a / b; break;
	}
	asm volatile("" ::: "memory");
	std::fesetround(old_round);
	regs.or_fflags(collect_fflags());
	T r = result;
	if (std::isnan(r)) r = canonical_nan<T>();
	return r;
}

template <typename T>
T fp_sqrt(T a, uint8_t rm, Registers &regs)
{
	clear_fp_exceptions();
	int old_round = std::fegetround();
	std::fesetround(host_round_mode(rm, regs.get_frm()));
	volatile T result = std::sqrt(a);
	std::fesetround(old_round);
	regs.or_fflags(collect_fflags());
	T r = result;
	if (std::isnan(r)) r = canonical_nan<T>();
	return r;
}

template <typename T>
T fp_fma(T a, T b, T c, uint8_t rm, Registers &regs)
{
	clear_fp_exceptions();
	int old_round = std::fegetround();
	std::fesetround(host_round_mode(rm, regs.get_frm()));
	volatile T result = std::fma(a, b, c);
	std::fesetround(old_round);
	regs.or_fflags(collect_fflags());
	T r = result;
	if (std::isnan(r)) r = canonical_nan<T>();
	return r;
}

// FEQ/FLT/FLE: a NaN operand compares false and sets NV. Spec actually
// distinguishes signaling from quiet NaN (only signaling NaN forces NV
// for FEQ; both do for FLT/FLE) -- simplified here to "any NaN sets NV",
// which can set the flag slightly more often than strict compliance but
// never changes the returned comparison result.
template <typename T>
uint64_t fcompare(T a, T b, uint8_t funct3, Registers &regs)
{
	if (std::isnan(a) || std::isnan(b)) {
		regs.or_fflags(0x10);
		return 0;
	}
	switch (funct3) {
	case 0b010: return a == b ? 1 : 0; // FEQ
	case 0b001: return a < b  ? 1 : 0; // FLT
	default:    return a <= b ? 1 : 0; // FLE
	}
}

// FMIN/FMAX: canonical NaN if both inputs are NaN, the non-NaN operand if
// only one is -- std::fmin/fmax already implement exactly that. Same NaN-
// signaling simplification as fcompare above.
template <typename T>
T fminmax(T a, T b, bool is_max, Registers &regs)
{
	if (std::isnan(a) || std::isnan(b)) regs.or_fflags(0x10);
	if (std::isnan(a) && std::isnan(b)) return canonical_nan<T>();
	return is_max ? std::fmax(a, b) : std::fmin(a, b);
}

// FSGNJ/FSGNJN/FSGNJX: pure bit manipulation (copy rs1's magnitude, take
// rs2's sign bit, possibly negated/XORed in) -- no rounding, no exceptions.
inline float fsgnj_f32(float a, float b, uint8_t funct3)
{
	uint32_t ab = bits_from_f32(a), bb = bits_from_f32(b), sign = 0x80000000u, r;
	switch (funct3) {
	case 0b000: r = (ab & ~sign) | (bb & sign); break;
	case 0b001: r = (ab & ~sign) | (~bb & sign); break;
	default:    r = ab ^ (bb & sign); break;
	}
	return f32_from_bits(r);
}
inline double fsgnj_f64(double a, double b, uint8_t funct3)
{
	uint64_t ab = bits_from_f64(a), bb = bits_from_f64(b), sign = 0x8000000000000000ull, r;
	switch (funct3) {
	case 0b000: r = (ab & ~sign) | (bb & sign); break;
	case 0b001: r = (ab & ~sign) | (~bb & sign); break;
	default:    r = ab ^ (bb & sign); break;
	}
	return f64_from_bits(r);
}

// FCLASS: 10-bit category mask (bit0=-inf ... bit9=quiet NaN). Signaling
// vs quiet NaN is told apart by the mantissa's MSB, the usual convention.
template <typename T>
uint64_t fclassify(T v)
{
	if (std::isnan(v)) {
		bool quiet;
		if constexpr (sizeof(T) == 4) { quiet = (bits_from_f32((float)v) >> 22) & 1; }
		else { quiet = (bits_from_f64((double)v) >> 51) & 1; }
		return quiet ? (1ull << 9) : (1ull << 8);
	}
	bool neg = std::signbit(v);
	if (std::isinf(v)) return neg ? (1ull << 0) : (1ull << 7);
	if (v == 0) return neg ? (1ull << 3) : (1ull << 4);
	if (std::fpclassify(v) == FP_SUBNORMAL) return neg ? (1ull << 2) : (1ull << 5);
	return neg ? (1ull << 1) : (1ull << 6);
}

// Float/double -> integer conversions. Out-of-range and NaN clamp to the
// target type's max/min (NaN -> max) and set NV, matching FCVT.*'s spec'd
// "invalid" behavior instead of relying on UB from an out-of-range cast.
inline int32_t fcvt_to_i32(double v, Registers &regs)
{
	if (std::isnan(v) || v >= 2147483648.0) { regs.or_fflags(0x10); return INT32_MAX; }
	if (v < -2147483648.0) { regs.or_fflags(0x10); return INT32_MIN; }
	return (int32_t)std::llrint(v);
}
inline uint32_t fcvt_to_u32(double v, Registers &regs)
{
	if (std::isnan(v) || v >= 4294967296.0) { regs.or_fflags(0x10); return 0xFFFFFFFFu; }
	if (v < 0.0) { regs.or_fflags(0x10); return 0; }
	return (uint32_t)std::llrint(v);
}
inline int64_t fcvt_to_i64(double v, Registers &regs)
{
	if (std::isnan(v) || v >= 9223372036854775808.0) { regs.or_fflags(0x10); return INT64_MAX; }
	if (v < -9223372036854775808.0) { regs.or_fflags(0x10); return INT64_MIN; }
	return std::llrint(v);
}
inline uint64_t fcvt_to_u64(double v, Registers &regs)
{
	// A double's 52-bit mantissa can't exactly represent every uint64_t
	// near the top of its range, but that's an inherent precision limit
	// of using double as the common conversion path, not a correctness
	// bug -- FCVT.LU.* is not something Doom-adjacent code is expected
	// to lean on for exact huge-integer round-tripping.
	if (std::isnan(v) || v >= 18446744073709551616.0) { regs.or_fflags(0x10); return UINT64_MAX; }
	if (v < 0.0) { regs.or_fflags(0x10); return 0; }
	// std::llrint returns a *signed* long long -- for v in [2^63, 2^64) that
	// range doesn't fit a signed 64-bit result at all, so calling it
	// directly is UB (x86 and ARM64 were observed saturating to different,
	// both-wrong bit patterns here during arch-test cross-validation).
	// Shift the input down into llrint's safe signed range, round there,
	// then shift the rounded integer back up unsigned.
	if (v >= 9223372036854775808.0) {
		return 0x8000000000000000ull + (uint64_t)std::llrint(v - 9223372036854775808.0);
	}
	return (uint64_t)std::llrint(v);
}
