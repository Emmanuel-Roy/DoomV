// The scalar floating-point unit: F, D, Zfh, Zfhmin and Zfa, one
// implementation for every format. See fpu.hpp for the design.
//
// The encodings make this possible. Within OP-FP, funct7's top five bits name
// the operation and its low two bits the format (00 S, 01 D, 10 H); the fused
// multiply-adds carry the same format field as funct2. FADD.S, FADD.D and
// FADD.H are one instruction at three widths, and so are the conversions, the
// moves, the comparisons and Zfa's additions. Which encodings exist on this
// machine is the decoder's decision (Zfh, Zfhmin, -march), made before this
// runs; here an operation is an operation.
#include "fpu.hpp"
#include "riscv_decoder.hpp"
#include "registers.hpp"

namespace fpu {
namespace {

// Zfa's FLI: the 32 constants, as doubles and as singles. Index 1 is each
// format's own smallest normal; the others are the same numbers in both.
constexpr uint64_t FLI_D[32] = {
	0xBFF0000000000000ull, 0x0010000000000000ull, 0x3EF0000000000000ull, 0x3F00000000000000ull, // -1, min normal, 2^-16, 2^-15
	0x3F70000000000000ull, 0x3F80000000000000ull, 0x3FB0000000000000ull, 0x3FC0000000000000ull, // 2^-8, 2^-7, 2^-4, 2^-3
	0x3FD0000000000000ull, 0x3FD4000000000000ull, 0x3FD8000000000000ull, 0x3FDC000000000000ull, // 0.25 .. 0.4375
	0x3FE0000000000000ull, 0x3FE4000000000000ull, 0x3FE8000000000000ull, 0x3FEC000000000000ull, // 0.5 .. 0.875
	0x3FF0000000000000ull, 0x3FF4000000000000ull, 0x3FF8000000000000ull, 0x3FFC000000000000ull, // 1 .. 1.75
	0x4000000000000000ull, 0x4004000000000000ull, 0x4008000000000000ull, 0x4010000000000000ull, // 2, 2.5, 3, 4
	0x4020000000000000ull, 0x4030000000000000ull, 0x4060000000000000ull, 0x4070000000000000ull, // 8, 16, 128, 256
	0x40E0000000000000ull, 0x40F0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull, // 2^15, 2^16, inf, NaN
};
constexpr uint32_t FLI_S[32] = {
	0xBF800000u, 0x00800000u, 0x37800000u, 0x38000000u,
	0x3B800000u, 0x3C000000u, 0x3D800000u, 0x3E000000u,
	0x3E800000u, 0x3EA00000u, 0x3EC00000u, 0x3EE00000u,
	0x3F000000u, 0x3F200000u, 0x3F400000u, 0x3F600000u,
	0x3F800000u, 0x3FA00000u, 0x3FC00000u, 0x3FE00000u,
	0x40000000u, 0x40200000u, 0x40400000u, 0x40800000u,
	0x41000000u, 0x41800000u, 0x43000000u, 0x43800000u,
	0x47000000u, 0x47800000u, 0x7F800000u, 0x7FC00000u,
};

inline uint64_t sext(uint64_t v, int width)
{
	return width >= 64 ? v : (uint64_t)((int64_t)(v << (64 - width)) >> (64 - width));
}

// Zfa's FCVTMOD.W.D: truncate toward zero, then wrap modulo 2^32 rather than
// saturate -- the conversion a JavaScript engine needs. NaN and infinity give
// zero and raise invalid; out of range raises invalid, and invalid replaces
// inexact rather than joining it. In SoftFloat: truncate (roundToInt, which
// reports a discarded fraction as inexact), range-check by comparison, and
// reduce modulo 2^32 with the IEEE remainder, which is exact -- so the huge
// inputs this exists for never meet an integer conversion that could not
// hold them.
uint64_t fcvtmod_w_d(float64_t a)
{
	if (classify(a) & 0x381) {   // -inf (bit 0), +inf (7), a NaN (8, 9)
		raise(softfloat_flag_invalid);
		return 0;
	}
	const uint_fast8_t before = softfloat_exceptionFlags;
	softfloat_exceptionFlags = 0;
	const float64_t t = f64_roundToInt(a, softfloat_round_minMag, true);
	const bool inexact = (softfloat_exceptionFlags & softfloat_flag_inexact) != 0;
	const bool in_range = f64_le(make<float64_t>(0xC1E0000000000000ull), t)    // -2^31
	                   && f64_le(t, make<float64_t>(0x41DFFFFFFFC00000ull));   // 2^31 - 1
	const float64_t r = f64_rem(t, make<float64_t>(0x41F0000000000000ull));    // t mod 2^32, in [-2^31, 2^31]
	const int64_t w = f64_to_i64(r, softfloat_round_minMag, false);
	softfloat_exceptionFlags = before | (!in_range ? softfloat_flag_invalid
	                                               : inexact ? softfloat_flag_inexact : 0);
	return sext((uint32_t)w, 32);
}

template <class T> void execute_format(const DecodedOp &in, Registers &regs)
{
	constexpr int W = Fmt<T>::width;
	const uint8_t frm = regs.get_frm();
	const uint_fast8_t rm = sf::round_mode(in.funct3, frm);
	auto src = [&](int r) { return read<T>(regs, r); };

	// The fused multiply-adds. FMSUB negates the addend, FNMSUB the product,
	// FNMADD both; the negation is applied to the operands going in (a sign
	// flip of the bits, which leaves a signaling NaN signaling) so the
	// fusion stays a single rounding.
	switch (in.opcode) {
	case 0b1000011: case 0b1000111: case 0b1001011: case 0b1001111: {
		T a = src(in.rs1), c = src(in.rs3);
		const T b = src(in.rs2);
		if (in.opcode == 0b1000111 || in.opcode == 0b1001111) c = negate(c);
		if (in.opcode == 0b1001011 || in.opcode == 0b1001111) a = negate(a);
		sf::begin(in.funct3, frm);
		const T r = mul_add(a, b, c);
		sf::end(regs);
		write(regs, in.rd, r);
		return;
	}
	default: break;
	}

	switch (in.funct7 >> 2) {
	case 0x00: case 0x01: case 0x02: case 0x03: { // FADD FSUB FMUL FDIV
		const T a = src(in.rs1), b = src(in.rs2);
		sf::begin(in.funct3, frm);
		const unsigned op = in.funct7 >> 2;
		const T r = op == 0 ? add(a, b) : op == 1 ? sub(a, b) : op == 2 ? mul(a, b) : div(a, b);
		sf::end(regs);
		write(regs, in.rd, r);
		break;
	}
	case 0x0B: { // FSQRT
		const T a = src(in.rs1);
		sf::begin(in.funct3, frm);
		const T r = sqrt(a);
		sf::end(regs);
		write(regs, in.rd, r);
		break;
	}
	case 0x04: // FSGNJ FSGNJN FSGNJX
		write(regs, in.rd, sign_inject(src(in.rs1), src(in.rs2), in.funct3));
		break;
	case 0x05: { // FMIN FMAX, and Zfa's FMINM FMAXM (funct3 2, 3)
		const T a = src(in.rs1), b = src(in.rs2);
		sf::begin(0, frm);
		const T r = in.funct3 >= 2 ? min_max_nan(a, b, in.funct3 == 3) : min_max(a, b, in.funct3 == 1);
		sf::end(regs);
		write(regs, in.rd, r);
		break;
	}
	case 0x14: { // FLE FLT FEQ, and Zfa's FLEQ FLTQ -- rd is an integer register
		const T a = src(in.rs1), b = src(in.rs2);
		sf::begin(0, frm);
		const bool r = compare(a, b, in.funct3);
		sf::end(regs);
		regs.write_x(in.rd, r ? 1 : 0);
		break;
	}
	case 0x08: { // between formats, rs2 the source: FCVT.S.D, .D.S, .H.S, ...; Zfa's FROUND(NX) at rs2 4, 5
		sf::begin(in.funct3, frm);
		T r{};
		switch (in.rs2) {
		case 0: r = convert<T>(read<float32_t>(regs, in.rs1)); break;
		case 1: r = convert<T>(read<float64_t>(regs, in.rs1)); break;
		case 2: r = convert<T>(read<float16_t>(regs, in.rs1)); break;
		default: r = round_to_int(src(in.rs1), rm, in.rs2 == 5); break;   // fround, froundnx (exact)
		}
		sf::end(regs);
		write(regs, in.rd, r);
		break;
	}
	case 0x18: { // to an integer: FCVT.W/WU/L/LU, and Zfa's FCVTMOD.W.D at rs2 8
		const T a = src(in.rs1);
		sf::begin(in.funct3, frm);
		uint64_t r = 0;
		switch (in.rs2) {
		case 0: r = sext((uint32_t)to_i32(a, rm), 32); break;
		case 1: r = sext(to_ui32(a, rm), 32); break;      // WU: sign-extended too, per spec
		case 2: r = (uint64_t)to_i64(a, rm); break;
		case 3: r = to_ui64(a, rm); break;
		case 8: if constexpr (W == 64) r = fcvtmod_w_d(a); break;
		default: break;
		}
		sf::end(regs);
		regs.write_x(in.rd, r);
		break;
	}
	case 0x1A: { // from an integer: FCVT.S/D/H.W/WU/L/LU
		const uint64_t x = regs.read_x(in.rs1);
		sf::begin(in.funct3, frm);
		T r{};
		switch (in.rs2) {
		case 0: r = from_i32<T>((int32_t)x); break;
		case 1: r = from_ui32<T>((uint32_t)x); break;
		case 2: r = from_i64<T>((int64_t)x); break;
		default: r = from_ui64<T>(x); break;
		}
		sf::end(regs);
		write(regs, in.rd, r);
		break;
	}
	case 0x1C: // FCLASS (funct3 1), or FMV.X.W/D/H: the raw low bits, sign-extended
		// A move takes the bits as they are, boxed or not -- it does not
		// interpret them as a number, so the unboxing rule does not apply.
		if (in.funct3 == 1) regs.write_x(in.rd, classify(src(in.rs1)));
		else regs.write_x(in.rd, sext(regs.read_f_bits(in.rs1) & (W == 64 ? ~0ull : (1ull << W) - 1), W));
		break;
	case 0x1E: // FMV.W/D/H.X: raw bits the other way, boxed; Zfa's FLI at rs2 1
		if (in.rs2 == 1) {
			if constexpr (W == 64) regs.write_f_bits(in.rd, FLI_D[in.rs1]);
			else if constexpr (W == 32) regs.write_f_bits(in.rd, box(FLI_S[in.rs1], 32));
			// FLI.H: not decoded on this machine
		} else {
			regs.write_f_bits(in.rd, box(regs.read_x(in.rs1), W));
		}
		break;
	default:
		break; // the decoder gates unrecognized combinations to illegal first
	}
}

} // namespace

void execute(const DecodedOp &instr, Registers &regs)
{
	switch (instr.funct7 & 3) {
	case 0: execute_format<float32_t>(instr, regs); break;
	case 1: execute_format<float64_t>(instr, regs); break;
	case 2: execute_format<float16_t>(instr, regs); break;
	default: break; // 11 is quad precision, which this machine does not have
	}
}

} // namespace fpu
