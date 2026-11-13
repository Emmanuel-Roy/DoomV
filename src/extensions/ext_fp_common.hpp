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
#include "ext_softfloat.hpp"

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

// The same, as bit patterns, for code that hands them to SoftFloat: nothing
// on the way can turn a signaling NaN into a quiet one.
inline uint32_t read_f32_bits(Registers &r, int i)
{
	const uint64_t b = bits_from_f64(r.read_f(i));
	return ((b >> 32) == 0xFFFFFFFFull) ? (uint32_t)b : 0x7FC00000u;
}
inline void write_f32_bits(Registers &r, int i, uint32_t bits) { r.write_f(i, f64_from_bits(box_f32(bits))); }

// RISC-V's rm field: 000=RNE,001=RTZ,010=RDN,011=RUP,100=RMM,111=dynamic
// (use fcsr's frm), as a host <cfenv> mode. The host has no "round to
// nearest, ties away from zero", so RMM comes out as ties-to-even: wrong on
// every exact tie. Nothing that rounds may use this for that reason -- scalar
// F/D/Zfa/Zfhmin rounding all goes through SoftFloat, which has RMM (BUGS
// 193). What is left is vector code that sets the host mode around work
// SoftFloat now does, where the mode no longer reaches a result.
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
// Clearing and collecting have to cover the same state, or a flag raised
// by one unit is read back through the other and attributed to whichever
// operation happened to look next. Both go through <cfenv>, which on this
// toolchain covers the x87 status word *and* MXCSR; the MXCSR clear is kept
// alongside it because feclearexcept is not guaranteed to touch it.
inline void clear_fp_exceptions()
{
	_mm_setcsr(_mm_getcsr() & ~0x3Fu); // IE/DE/ZE/OE/UE/PE (MXCSR bits 0-5)
	std::feclearexcept(FE_ALL_EXCEPT);
}

// fflags bit positions (NV/DZ/OF/UF/NX). "Denormal operand" has no RISC-V
// equivalent and is intentionally dropped.
//
// This reads <cfenv> rather than MXCSR directly, and the difference is not
// cosmetic. Not every libm routine computes in SSE: on this toolchain
// double-precision fma() is a software/x87 implementation that never
// touches MXCSR, so an MXCSR-only read reported *no exception at all* for
// every FMADD/FMSUB/FNMADD/FNMSUB.D -- inexact and invalid alike. Single
// precision fmaf() happened to go through SSE and looked fine, which is
// what made it survive so long. std::llrint had already been caught doing
// the same thing, and was worked around one call site at a time; this fixes
// the shared cause instead.
//
// Found by riscv-arch-test D-fnmadd.d-* against Sail.
//
// Call this BEFORE restoring the rounding mode, never after. Every caller
// here runs clear -> fesetround(guest mode) -> compute -> collect ->
// fesetround(old), and the last two used to be the other way round. C says
// fesetround establishes the rounding direction and says nothing that
// guarantees it leaves the exception flags alone; whether it does is the
// runtime's business. The GCC 8.1 bundle's mingw-w64 does a read-modify-write
// and preserves them, so the wrong order worked here for as long as there was
// only one toolchain. llvm-mingw's writes MXCSR whole and clears the status
// bits with it, which silently zeroed NX on every FCVT and VFNCVT -- ten
// riscv-arch-test failures and three vector ones, all of them fflags, none of
// them values. Collecting first is correct under either, and costs nothing.
inline uint8_t collect_fflags()
{
	int e = std::fetestexcept(FE_ALL_EXCEPT);
	uint8_t flags = 0;
	if (e & FE_INVALID)   flags |= 0x10; // NV
	if (e & FE_DIVBYZERO) flags |= 0x08; // DZ
	if (e & FE_OVERFLOW)  flags |= 0x04; // OF
	if (e & FE_UNDERFLOW) flags |= 0x02; // UF
	if (e & FE_INEXACT)   flags |= 0x01; // NX
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
// The three arithmetic primitives, computed in software.
//
// These took the host FPU's answer and the host FPU's status register
// until riscv-arch-test showed both to be unreliable here -- see the
// header of ext_softfloat.hpp for what specifically, and why no amount of
// wrapper adjustment fixes either. SoftFloat is the same library spike
// computes F/D with, and Sail's exactness has the same root cause: neither
// touches a host FPU.
//
// Values arrive and leave as host float/double because that is what the
// register file and the rest of the emulator still speak; only the
// arithmetic itself moves. The bit patterns are identical either way, so
// this is a representation change at the boundary, not a semantic one.
//
// NaN canonicalisation is no longer done here. SoftFloat's RISC-V
// specialization already produces 0x7FC00000 / 0x7FF8000000000000 as its
// default NaN, so doing it again would be dead code that looks load-bearing.
template <typename T>
T fp_binop(T a, T b, char op, uint8_t rm, Registers &regs)
{
	sf::begin(rm, regs.get_frm());
	if constexpr (sizeof(T) == 4) {
		float32_t x = sf::f32(bits_from_f32((float)a));
		float32_t y = sf::f32(bits_from_f32((float)b));
		float32_t r = x;
		switch (op) {
		case '+': r = f32_add(x, y); break;
		case '-': r = f32_sub(x, y); break;
		case '*': r = f32_mul(x, y); break;
		case '/': r = f32_div(x, y); break;
		}
		sf::end(regs);
		return (T)f32_from_bits(sf::bits(r));
	} else {
		float64_t x = sf::f64(bits_from_f64((double)a));
		float64_t y = sf::f64(bits_from_f64((double)b));
		float64_t r = x;
		switch (op) {
		case '+': r = f64_add(x, y); break;
		case '-': r = f64_sub(x, y); break;
		case '*': r = f64_mul(x, y); break;
		case '/': r = f64_div(x, y); break;
		}
		sf::end(regs);
		return (T)f64_from_bits(sf::bits(r));
	}
}

template <typename T>
T fp_sqrt(T a, uint8_t rm, Registers &regs)
{
	sf::begin(rm, regs.get_frm());
	if constexpr (sizeof(T) == 4) {
		float32_t r = f32_sqrt(sf::f32(bits_from_f32((float)a)));
		sf::end(regs);
		return (T)f32_from_bits(sf::bits(r));
	} else {
		float64_t r = f64_sqrt(sf::f64(bits_from_f64((double)a)));
		sf::end(regs);
		return (T)f64_from_bits(sf::bits(r));
	}
}

// Fused multiply-add: one rounding for the whole a*b+c, which is the
// entire point of the instruction and the thing a separate multiply and
// add cannot reproduce.
template <typename T>
T fp_fma(T a, T b, T c, uint8_t rm, Registers &regs)
{
	sf::begin(rm, regs.get_frm());
	if constexpr (sizeof(T) == 4) {
		float32_t r = f32_mulAdd(sf::f32(bits_from_f32((float)a)),
		                         sf::f32(bits_from_f32((float)b)),
		                         sf::f32(bits_from_f32((float)c)));
		sf::end(regs);
		return (T)f32_from_bits(sf::bits(r));
	} else {
		float64_t r = f64_mulAdd(sf::f64(bits_from_f64((double)a)),
		                         sf::f64(bits_from_f64((double)b)),
		                         sf::f64(bits_from_f64((double)c)));
		sf::end(regs);
		return (T)f64_from_bits(sf::bits(r));
	}
}

// A NaN is signalling when the MSB of its significand is clear. Which NaNs
// raise invalid is not a detail: it is the only thing separating FEQ from
// FLT/FLE, and Zfa's fleq/fltq from those again.
inline bool is_snan_f32(float f)
{
	uint32_t b = bits_from_f32(f);
	return ((b & 0x7F800000u) == 0x7F800000u) && (b & 0x007FFFFFu) && !(b & 0x00400000u);
}

inline bool is_snan_f64(double d)
{
	uint64_t b = bits_from_f64(d);
	return ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
	    && (b & 0x000FFFFFFFFFFFFFull) && !(b & 0x0008000000000000ull);
}

template <typename T> bool is_snan(T v);
template <> inline bool is_snan<float>(float v)   { return is_snan_f32(v); }
template <> inline bool is_snan<double>(double v) { return is_snan_f64(v); }

// FEQ/FLT/FLE: a NaN operand compares false, but which NaNs set NV differs
// between them -- FEQ is a quiet comparison and raises only for a signalling
// NaN, while FLT/FLE are signalling comparisons and raise for any NaN.
//
// This used to be simplified to "any NaN sets NV" on the grounds that it
// never changes the returned value. True, and still misleading: the flag
// *is* the observable difference between FEQ and FLT, and between both and
// Zfa's fleq/fltq. A differential test dumping fflags per operation catches
// it at once; the one dumping only accrued flags at the end did not, because
// other instructions in the same test set NV legitimately and masked it.
template <typename T>
uint64_t fcompare(T a, T b, uint8_t funct3, Registers &regs)
{
	if (std::isnan(a) || std::isnan(b)) {
		bool quiet_form = (funct3 == 0b010); // FEQ
		if (!quiet_form || is_snan(a) || is_snan(b)) regs.or_fflags(0x10);
		return 0;
	}
	switch (funct3) {
	case 0b010: return a == b ? 1 : 0; // FEQ
	case 0b001: return a < b  ? 1 : 0; // FLT
	default:    return a <= b ? 1 : 0; // FLE
	}
}

// FMIN/FMAX: canonical NaN if both inputs are NaN, the non-NaN operand if
// only one is.
//
// Only a signalling NaN raises invalid here. A quiet NaN operand is the
// ordinary, non-exceptional case these instructions are built around.
//
// Every case is decided here rather than left to std::fmin/fmax. C puts a
// signaling NaN outside its rules, and glibc returns it quieted -- fmin(0,
// 0x7F800001) is 0x7FC00001 -- where RISC-V wants the other operand. MinGW
// happened to return the operand, so the Windows build passed and a Linux one
// would not (BUGS 194). The ordering is SoftFloat's quiet less-than, as in
// spike.
template <typename T>
T fminmax(T a, T b, bool is_max, Registers &regs)
{
	if (is_snan(a) || is_snan(b)) regs.or_fflags(0x10);
	const bool a_nan = std::isnan(a), b_nan = std::isnan(b);
	if (a_nan && b_nan) return canonical_nan<T>();
	if (a_nan) return b;
	if (b_nan) return a;

	// Zeros of opposite sign compare equal. RISC-V orders them: fmin(+0,-0)
	// is -0.0 and fmax(+0,-0) is +0.0, regardless of operand order. Decide it
	// from the sign bits instead of the comparison.
	if (a == (T)0 && b == (T)0) {
		bool negative = is_max ? (std::signbit(a) && std::signbit(b))
		                       : (std::signbit(a) || std::signbit(b));
		return negative ? -(T)0 : (T)0;
	}

	bool lt;
	if constexpr (sizeof(T) == 4) lt = f32_lt_quiet(sf::f32(bits_from_f32((float)a)), sf::f32(bits_from_f32((float)b)));
	else lt = f64_lt_quiet(sf::f64(bits_from_f64((double)a)), sf::f64(bits_from_f64((double)b)));
	return is_max ? (lt ? b : a) : (lt ? a : b);
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
