#pragma once
// Berkeley SoftFloat, wrapped for DoomV's F/D paths.
//
// Why software floating point at all, when the host has an FPU
// -----------------------------------------------------------
// Computing on the host FPU was the original design, and it is fast and
// almost always right. "Almost" turned out to have two hard edges, both
// found by riscv-arch-test against Sail rather than by anything
// hand-written:
//
//   1. RMM cannot be expressed on x86. RISC-V has five rounding modes;
//      MXCSR has four. Round-to-nearest-ties-away has no encoding, so it
//      was approximated by round-to-nearest-even, which differs on exactly
//      the cases a conformance test aims at -- every exact tie, off by one
//      ulp. A single arch-test file contains 390 RMM cases.
//
//   2. Host exception flags are not trustworthy here. This machine is
//      ARM64 running x86-64 binaries under Windows' Prism translation
//      layer, where MXCSR is emulated rather than silicon and NX/UF do not
//      reliably survive. Separately, MinGW's double-precision fma() is a
//      software routine that never touches MXCSR at all, so *every*
//      FMADD/FMSUB/FNMADD/FNMSUB.D reported no exceptions whatsoever.
//
// Both are properties of computing on someone else's FPU and reading its
// status register. Neither is fixable by adjusting the wrapper. Sail and
// spike are exact for the same reason as each other: they never touch a
// host FPU. This does the same.
//
// The library is spike's own vendored copy rather than a second checkout,
// so the two cannot drift apart.
//
// Two things make this integration much smaller than it looks. SoftFloat's
// rounding enum is numerically identical to the RISC-V rm field:
//
//     0 RNE   near_even        3 RUP   max
//     1 RTZ   minMag           4 RMM   near_maxMag
//     2 RDN   min
//
// and its exception flags are numerically identical to fflags:
//
//     1 NX inexact   2 UF underflow   4 OF overflow
//     8 DZ infinite  16 NV invalid
//
// Neither is a coincidence -- SoftFloat carries a RISC-V specialization --
// so both are passed straight through rather than translated. A translation
// table here would be code that looks meaningful and can only introduce
// bugs.
#include <cstdint>

extern "C" {
#include "softfloat.h"
}

#include "registers.hpp"

namespace sf {

// RISC-V's rm field, resolved through frm for the dynamic encoding.
//
// An invalid or reserved mode is the caller's problem, not this layer's:
// the decoder raises an illegal instruction for rm=5/6 and for a reserved
// frm, so anything arriving here is one of the five real modes.
inline uint_fast8_t round_mode(uint8_t rm, uint8_t frm)
{
	return (uint_fast8_t)((rm == 0b111) ? frm : rm);
}

// Install the rounding mode and clear the flag accumulator before an
// operation. SoftFloat accumulates into a global rather than returning
// flags, so it has to be cleared per operation or one instruction's
// exceptions are reported by the next.
inline void begin(uint8_t rm, uint8_t frm)
{
	softfloat_roundingMode = round_mode(rm, frm);
	softfloat_exceptionFlags = 0;
}

// Fold whatever the operation raised into fflags. The bit values already
// match, so this is an OR and not a mapping.
inline void end(Registers &regs)
{
	regs.or_fflags((uint8_t)softfloat_exceptionFlags);
}

// SoftFloat carries its own float32_t/float64_t structs so that a host
// float is never accidentally substituted for one. These convert at the
// boundary, where DoomV still holds raw bit patterns in the register file.
// Half joins for the same reason single and double did: SoftFloat's f16
// routines are exact and the hand-written narrowing in ext_fp16.hpp only
// ever covered conversion, which is all Zfhmin needs. Full Zfh arithmetic
// would have meant writing a second rounding implementation, and the one
// lesson of the F/D work is that rounding written twice rounds differently.
inline float16_t f16(uint16_t bits) { float16_t v; v.v = bits; return v; }
inline uint16_t bits(float16_t v)   { return (uint16_t)v.v; }
inline float32_t f32(uint32_t bits) { float32_t v; v.v = bits; return v; }
inline float64_t f64(uint64_t bits) { float64_t v; v.v = bits; return v; }
inline uint32_t bits(float32_t v)   { return (uint32_t)v.v; }
inline uint64_t bits(float64_t v)   { return (uint64_t)v.v; }

} // namespace sf
