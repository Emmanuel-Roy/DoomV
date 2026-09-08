// Zfh: full half-precision arithmetic.
//
// Zfhmin, next door, deliberately stops at load/store/move/convert -- that
// is all RVA23U64 mandates, and the design intent is that a guest wanting
// half arithmetic widens to single, computes, and narrows back. Zfh is the
// expansion option that lets it compute in half directly, and it is what
// riscv-tests exercises in its rv64uzfh family.
//
// Everything numeric goes through Berkeley SoftFloat's f16 routines, for the
// same reason F and D do -- see ext_softfloat.hpp, which lays out why
// computing on the host FPU is not trustworthy here. It matters more for
// half than for either: the format has an 11-bit significand, so a
// double-rounding error from computing in single and narrowing is not a
// corner case but a routine one, and writing a second rounding
// implementation by hand is precisely the mistake that wrapper exists to
// avoid.
//
// Encoding, from OP-FP's funct7: bits 26:25 are the format field, and half
// is 0b10 (single 00, double 01, quad 11). So every half-precision op is its
// single-precision counterpart's funct7 with those two bits set to 10, and
// the same holds for the fused-multiply opcodes' funct2. That regularity is
// why classify() can route the whole extension on one test.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "ext_fp_common.hpp"
#include "ext_fp16.hpp"
#include "ext_softfloat.hpp"
#include "ext_xstate.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// The register file holds doubles; a half sits NaN-boxed in the low 16 bits.
// Reading for arithmetic *does* apply the unbox rule -- an improperly boxed
// pattern reads as the canonical NaN -- because these instructions
// interpret the value as a number, unlike the moves and stores in Zfhmin.
inline uint16_t rd_h(Registers &regs, int r)
{
	return fp16::unbox_f16(bits_from_f64(regs.read_f(r)));
}

inline void wr_h(Registers &regs, int r, uint16_t h)
{
	regs.write_f(r, f64_from_bits(fp16::box_f16(h)));
}

// fmin/fmax follow the same rules as their single and double counterparts,
// which are not SoftFloat's: a quiet NaN operand is ignored rather than
// propagated, two NaNs give the canonical NaN, and -0.0 compares less than
// +0.0 even though they are numerically equal. SoftFloat has no f16_min, so
// this is written out.
constexpr uint16_t H_CANON_NAN = 0x7E00;

inline bool h_is_zero(uint16_t h) { return (h & 0x7FFF) == 0; }

uint16_t h_minmax(uint16_t a, uint16_t b, bool want_max, uint8_t &flags)
{
	const bool a_nan = fp16::h_is_nan(a), b_nan = fp16::h_is_nan(b);
	// A signalling NaN raises invalid even when the result comes from the
	// other operand -- the flag reports what was *seen*, not what was used.
	if (fp16::h_is_snan(a) || fp16::h_is_snan(b)) flags |= 0x10;
	if (a_nan && b_nan) return H_CANON_NAN;
	if (a_nan) return b;
	if (b_nan) return a;
	if (h_is_zero(a) && h_is_zero(b)) {
		// Both zero: sign decides, so min(-0,+0) is -0 and max is +0.
		const bool a_neg = (a >> 15) != 0;
		return want_max ? (a_neg ? b : a) : (a_neg ? a : b);
	}
	const bool a_lt = f16_lt(sf::f16(a), sf::f16(b));
	return want_max ? (a_lt ? b : a) : (a_lt ? a : b);
}

} // namespace

DecodedInstruction Decoder::decode_zfh(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZFH;
	instr.length = 4;
	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;
	instr.rs3    = (raw_instr >> 27) & 0x1F;

	switch (instr.opcode) {
	case 0b1000011: instr.mnemonic = "FMADD.H";  return instr;
	case 0b1000111: instr.mnemonic = "FMSUB.H";  return instr;
	case 0b1001011: instr.mnemonic = "FNMSUB.H"; return instr;
	case 0b1001111: instr.mnemonic = "FNMADD.H"; return instr;
	default: break;
	}

	switch (instr.funct7) {
	case 0x02: instr.mnemonic = "FADD.H"; break;
	case 0x06: instr.mnemonic = "FSUB.H"; break;
	case 0x0A: instr.mnemonic = "FMUL.H"; break;
	case 0x0E: instr.mnemonic = "FDIV.H"; break;
	case 0x2E: instr.mnemonic = "FSQRT.H"; break;
	case 0x12: instr.mnemonic = instr.funct3 == 0 ? "FSGNJ.H"
	                          : instr.funct3 == 1 ? "FSGNJN.H" : "FSGNJX.H"; break;
	case 0x16: instr.mnemonic = instr.funct3 == 0 ? "FMIN.H" : "FMAX.H"; break;
	case 0x52: instr.mnemonic = instr.funct3 == 2 ? "FEQ.H"
	                          : instr.funct3 == 1 ? "FLT.H" : "FLE.H"; break;
	case 0x72: instr.mnemonic = "FCLASS.H"; break;
	case 0x62: instr.mnemonic = "FCVT.W.H"; break;
	case 0x6A: instr.mnemonic = "FCVT.H.W"; break;
	default:   instr.ext = Extension::ILLEGAL; break;
	}
	return instr;
}

void RiscvCore::exec_ZFH(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	const uint8_t frm = regs.get_frm();

	// The fused forms first: they are a separate opcode rather than a funct7,
	// and their sign manipulation is applied to the operands going in rather
	// than to the result coming out, so that the fusion stays single-rounded.
	switch (instr.opcode) {
	case 0b1000011: case 0b1000111: case 0b1001011: case 0b1001111: {
		uint16_t a = rd_h(regs, instr.rs1);
		uint16_t b = rd_h(regs, instr.rs2);
		uint16_t c = rd_h(regs, instr.rs3);
		// FMSUB negates the addend, FNMADD negates the product and the
		// addend, FNMSUB negates the product only.
		if (instr.opcode == 0b1000111 || instr.opcode == 0b1001111) c ^= 0x8000;
		if (instr.opcode == 0b1001011 || instr.opcode == 0b1001111) a ^= 0x8000;
		sf::begin(instr.funct3, frm);
		uint16_t r = sf::bits(f16_mulAdd(sf::f16(a), sf::f16(b), sf::f16(c)));
		sf::end(regs);
		wr_h(regs, instr.rd, r);
		vcommon::mark_fp_dirty(regs);
		regs.set_pc(regs.get_pc() + instr.length);
		return;
	}
	default: break;
	}

	switch (instr.funct7) {
	case 0x02: case 0x06: case 0x0A: case 0x0E: {
		const uint16_t a = rd_h(regs, instr.rs1);
		const uint16_t b = rd_h(regs, instr.rs2);
		sf::begin(instr.funct3, frm);
		uint16_t r;
		switch (instr.funct7) {
		case 0x02: r = sf::bits(f16_add(sf::f16(a), sf::f16(b))); break;
		case 0x06: r = sf::bits(f16_sub(sf::f16(a), sf::f16(b))); break;
		case 0x0A: r = sf::bits(f16_mul(sf::f16(a), sf::f16(b))); break;
		default:   r = sf::bits(f16_div(sf::f16(a), sf::f16(b))); break;
		}
		sf::end(regs);
		wr_h(regs, instr.rd, r);
		break;
	}

	case 0x2E: {
		const uint16_t a = rd_h(regs, instr.rs1);
		sf::begin(instr.funct3, frm);
		uint16_t r = sf::bits(f16_sqrt(sf::f16(a)));
		sf::end(regs);
		wr_h(regs, instr.rd, r);
		break;
	}

	case 0x12: {
		// Sign injection is pure bit manipulation: no rounding, no flags,
		// and NaN operands pass through untouched rather than being
		// canonicalised, which is why this does not go through SoftFloat.
		const uint16_t a = rd_h(regs, instr.rs1);
		const uint16_t b = rd_h(regs, instr.rs2);
		uint16_t sign;
		switch (instr.funct3) {
		case 0:  sign = b & 0x8000; break;              // fsgnj
		case 1:  sign = (~b) & 0x8000; break;           // fsgnjn
		default: sign = (a ^ b) & 0x8000; break;        // fsgnjx
		}
		wr_h(regs, instr.rd, (uint16_t)((a & 0x7FFF) | sign));
		break;
	}

	case 0x16: {
		uint8_t flags = 0;
		const uint16_t r = h_minmax(rd_h(regs, instr.rs1), rd_h(regs, instr.rs2),
		                            instr.funct3 == 1, flags);
		if (flags) regs.or_fflags(flags);
		wr_h(regs, instr.rd, r);
		break;
	}

	case 0x52: {
		// Comparisons write an integer register, so they take their own exit
		// rather than the FP-dirty tail below. feq is quiet -- only a
		// signalling NaN raises invalid; flt and fle are signalling, so any
		// NaN does.
		const uint16_t a = rd_h(regs, instr.rs1);
		const uint16_t b = rd_h(regs, instr.rs2);
		sf::begin(0, frm);
		bool result;
		if (instr.funct3 == 2)      result = f16_eq(sf::f16(a), sf::f16(b));
		else if (instr.funct3 == 1) result = f16_lt(sf::f16(a), sf::f16(b));
		else                        result = f16_le(sf::f16(a), sf::f16(b));
		sf::end(regs);
		regs.write_x(instr.rd, result ? 1 : 0);
		regs.set_pc(regs.get_pc() + instr.length);
		return;
	}

	case 0x72: {
		// fclass shares its funct7 with Zfhmin's fmv.x.h and is told apart
		// by funct3 -- 001 here, 000 there.
		const uint16_t h = rd_h(regs, instr.rs1);
		const uint16_t exp = (h >> 10) & 0x1F;
		const uint16_t man = h & 0x3FF;
		const bool neg = (h >> 15) != 0;
		uint64_t cls;
		if (exp == 0x1F && man == 0)      cls = neg ? (1u << 0) : (1u << 7);  // infinity
		else if (exp == 0x1F)             cls = (man & 0x200) ? (1u << 9) : (1u << 8); // qNaN/sNaN
		else if (exp == 0 && man == 0)    cls = neg ? (1u << 3) : (1u << 4);  // zero
		else if (exp == 0)                cls = neg ? (1u << 2) : (1u << 5);  // subnormal
		else                              cls = neg ? (1u << 1) : (1u << 6);  // normal
		regs.write_x(instr.rd, cls);
		regs.set_pc(regs.get_pc() + instr.length);
		return;
	}

	case 0x62: {
		// Half to integer. rs2 selects the width and signedness, exactly as
		// it does for the single and double forms.
		const uint16_t a = rd_h(regs, instr.rs1);
		sf::begin(instr.funct3, frm);
		uint64_t v;
		switch (instr.rs2) {
		case 0: v = (uint64_t)(int64_t)(int32_t)f16_to_i32(sf::f16(a),
		            sf::round_mode(instr.funct3, frm), true); break;
		case 1: v = (uint64_t)(int64_t)(int32_t)f16_to_ui32(sf::f16(a),
		            sf::round_mode(instr.funct3, frm), true); break;
		case 2: v = (uint64_t)f16_to_i64(sf::f16(a),
		            sf::round_mode(instr.funct3, frm), true); break;
		default: v = f16_to_ui64(sf::f16(a),
		            sf::round_mode(instr.funct3, frm), true); break;
		}
		sf::end(regs);
		regs.write_x(instr.rd, v);
		regs.set_pc(regs.get_pc() + instr.length);
		return;
	}

	case 0x6A: {
		// Integer to half. The narrow format makes rounding the common case
		// rather than the exception -- any integer above 2048 that is not a
		// multiple of its own ulp rounds, so this is exactly where computing
		// through single and narrowing would double-round.
		const uint64_t x = regs.read_x(instr.rs1);
		sf::begin(instr.funct3, frm);
		uint16_t r;
		switch (instr.rs2) {
		case 0: r = sf::bits(i32_to_f16((int32_t)x)); break;
		case 1: r = sf::bits(ui32_to_f16((uint32_t)x)); break;
		case 2: r = sf::bits(i64_to_f16((int64_t)x)); break;
		default: r = sf::bits(ui64_to_f16(x)); break;
		}
		sf::end(regs);
		wr_h(regs, instr.rd, r);
		break;
	}

	default:
		break;
	}

	vcommon::mark_fp_dirty(regs);
	regs.set_pc(regs.get_pc() + instr.length);
}
