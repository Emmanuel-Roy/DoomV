// OPFVV/OPFVF vector floating point: arithmetic, sign injection, min/max,
// compares, moves, merge and slides, conversions, sqrt/rsqrt7/rec7/class,
// the fused multiply-adds, the widening forms, and the reductions.
//
// The same unit as the scalar instructions (fpu.hpp): an element is a bit
// pattern of SEW bits, and only SoftFloat interprets it. Every operation is
// written once, as a template over the format, and run at the element width
// -- half (Zvfh), single or double -- by fpu::for_width.
//
// Elements used to travel through a host double, single and half alike, and
// that carrier was the source of a run of bugs: a host float<->double
// conversion quiets a signaling NaN, so a copy handed on a different NaN
// (BUGS 177), and arithmetic saw the NaN already quiet and never raised
// invalid (BUGS 195); half needed its own lossless NaN encoding inside the
// double to survive at all. With the bits as the element there is nothing to
// get wrong: a move copies them, and an operation hands them to SoftFloat.
//
// Vector FP has no rm field; every rounding is by frm (the dynamic mode), or
// toward zero for the .rtz and .rod conversions.
#include "riscv_decoder.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include "fpu.hpp"
#include <cstdint>

using namespace vcommon;

namespace {

using fpu::Fmt;

template <class T> inline T elem(Registers &regs, int vreg, uint64_t i)
{
	return fpu::make<T>(read_velem(regs, vreg, Fmt<T>::width, i));
}
template <class T> inline void set_elem(Registers &regs, int vreg, uint64_t i, T v)
{
	write_velem(regs, vreg, Fmt<T>::width, i, fpu::bits(v));
}

// One element's operation: rounded by frm (rm 7) or the given mode, its
// flags folded into fflags. Format conversions done on the way in -- the
// widening forms' operands -- belong inside, so their invalid is counted.
template <class F> inline auto rounded(Registers &regs, F &&f, uint8_t rm = 7)
{
	sf::begin(rm, regs.get_frm());
	auto r = f();
	sf::end(regs);
	return r;
}

// Conversions between floats and integers, every width: SoftFloat at the
// operands' own widths, one rounding. The integer is the float's width or
// half of it -- f64 to 64 or 32 bits, f32 to 32 or 16, f16 to 16 or 8 -- and
// SoftFloat has a conversion for each; the narrow ones saturate to the
// destination and raise invalid alone, as a conversion out of range does.
inline uint64_t vec_to_int(Registers &regs, uint64_t raw, int fsew, int isew, bool uns, bool rtz)
{
	sf::begin(rtz ? 1 : 7, regs.get_frm());
	const uint_fast8_t md = softfloat_roundingMode;
	uint64_t out = 0;
	fpu::for_width(fsew, [&](auto z) {
		using T = decltype(z);
		const T a = fpu::make<T>(raw);
		switch (isew) {
		case 64: out = uns ? fpu::to_ui64(a, md) : (uint64_t)fpu::to_i64(a, md); break;
		case 32: out = uns ? fpu::to_ui32(a, md) : (uint32_t)fpu::to_i32(a, md); break;
		case 16:
			if constexpr (Fmt<T>::width <= 32) out = uns ? fpu::to_ui16(a, md) : (uint16_t)fpu::to_i16(a, md);
			break;
		case 8:
			if constexpr (Fmt<T>::width == 16) out = uns ? fpu::to_ui8(a, md) : (uint8_t)fpu::to_i8(a, md);
			break;
		default: break;
		}
	});
	sf::end(regs);
	return out;
}
inline uint64_t vec_from_int(Registers &regs, uint64_t raw, int isew, int fsew, bool uns)
{
	const uint64_t u = isew == 64 ? raw : raw & ((1ull << isew) - 1);
	const int64_t v = isew == 64 ? (int64_t)raw : (int64_t)(u << (64 - isew)) >> (64 - isew);
	sf::begin(7, regs.get_frm());
	uint64_t out = 0;
	fpu::for_width(fsew, [&](auto z) {
		using T = decltype(z);
		out = fpu::bits(uns ? fpu::from_ui64<T>(u) : fpu::from_i64<T>(v));
	});
	sf::end(regs);
	return out;
}

// bf16, the brain-float format: f32's sign and eight exponent bits with
// seven of significand -- the top half of an f32. Zvfbfmin converts it,
// Zvfbfwma multiplies it into f32; there is no bf16 arithmetic. Both go
// through SoftFloat's bf16_to_f32 / f32_to_bf16.
inline bfloat16_t bf16(uint64_t b) { bfloat16_t v; v.v = (uint16_t)b; return v; }

} // namespace

namespace vcommon {

void exec_v_fp(const DecodedOp &instr, Registers &regs)
{
	const VType vt = decode_vtype(regs.get_vtype());
	const int sew = vt.sew;
	const uint64_t vl = regs.get_vl();
	const uint8_t funct6 = op_v_funct6(instr.funct7);
	const bool vm = op_v_vm(instr.funct7);
	const bool is_vv = (instr.funct3 == 0b001);

	// Half (Zvfh), single and double. SEW=8 is a float width for nothing, but
	// two conversions run at it with half on the other side: vfwcvt.f.x(u).v
	// widens an 8-bit integer to half, and vfncvt.x(u).f.w (and .rtz) narrows
	// half to an 8-bit integer. Those returned here, untouched -- found by
	// lock-stepping fp_softfloat.S against Sail, which converts them.
	const bool int8_conversion = sew == 8 && is_vv && funct6 == 0x12
		&& (instr.rs1 == 0x0A || instr.rs1 == 0x0B || instr.rs1 == 0x10 || instr.rs1 == 0x11
		    || instr.rs1 == 0x16 || instr.rs1 == 0x17);
	if (sew != 16 && sew != 32 && sew != 64 && !int8_conversion) return;

	// vfmv.f.s (.vv space: f[rd] = vs2[0]) and vfmv.s.f (.vf: vd[0] = f[rs1]).
	// A move: the element's bits, NaN-boxed into the f register; the scalar
	// read unboxed at the element width, as every .vf operand is.
	if (funct6 == 0x10) {
		if (is_vv) {
			regs.write_f_bits(instr.rd, fpu::box(read_velem(regs, instr.rs2, sew, 0), sew));
		} else if (vl > 0) {
			fpu::for_width(sew, [&](auto z) {
				using T = decltype(z);
				set_elem(regs, instr.rd, 0, fpu::read<T>(regs, instr.rs1));
			});
		}
		return;
	}

	// vfmerge.vfm (vm=0) / vfmv.v.f (vm=1): every element up to vl.
	if (!is_vv && funct6 == 0x17) {
		fpu::for_width(sew, [&](auto z) {
			using T = decltype(z);
			const T s = fpu::read<T>(regs, instr.rs1);
			for_each(regs, vl, [&](uint64_t i) {
				set_elem(regs, instr.rd, i, (vm || mask_bit(regs, i)) ? s : elem<T>(regs, instr.rs2, i));
			});
		});
		return;
	}

	// vfslide1up.vf / vfslide1down.vf: copies, the scalar at the element width.
	if (!is_vv && (funct6 == 0x0e || funct6 == 0x0f)) {
		fpu::for_width(sew, [&](auto z) {
			using T = decltype(z);
			const T s = fpu::read<T>(regs, instr.rs1);
			if (funct6 == 0x0e)
				for_each_active(regs, vm, vl, [&](uint64_t i) {
					set_elem(regs, instr.rd, i, i == 0 ? s : elem<T>(regs, instr.rs2, i - 1));
				});
			else
				for_each_active(regs, vm, vl, [&](uint64_t i) {
					set_elem(regs, instr.rd, i, i + 1 < vl ? elem<T>(regs, instr.rs2, i + 1) : s);
				});
		});
		return;
	}

	// Reductions: vfredusum, vfredosum, vfredmin, vfredmax. In order, element
	// by element -- for a sequential implementation the unordered sum is the
	// ordered one.
	if (is_vv && (funct6 == 0x01 || funct6 == 0x03 || funct6 == 0x05 || funct6 == 0x07)) {
		fpu::for_width(sew, [&](auto z) {
			using T = decltype(z);
			T acc = elem<T>(regs, instr.rs1, 0);
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				const T v = elem<T>(regs, instr.rs2, i);
				acc = rounded(regs, [&] {
					return funct6 == 0x05 ? fpu::min_max(acc, v, false)
					     : funct6 == 0x07 ? fpu::min_max(acc, v, true)
					                      : fpu::add(acc, v);
				});
			});
			if (vl > 0) set_elem(regs, instr.rd, 0, acc);
		});
		return;
	}

	// VFUNARY0's widening (vs1 0x08-0x0F) and narrowing (0x10-0x17) halves:
	// the wide side is 2*SEW. Each conversion is SoftFloat at both widths, so
	// it rounds once, from the source, and a NaN becomes the destination's
	// canonical NaN with invalid for a signaling one.
	if (is_vv && funct6 == 0x12 && instr.rs1 >= 0x08) {
		const uint8_t sub = instr.rs1;

		if (sub == 0x0D && sew == 16) {   // vfwcvtbf16.f.f.v
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				bfloat16_t b;
				b.v = (uint16_t)read_velem(regs, instr.rs2, 16, i);
				write_velem(regs, instr.rd, 32, i, fpu::bits(rounded(regs, [&] { return bf16_to_f32(b); })));
			});
			return;
		}
		if (sub == 0x1D && sew == 16) {   // vfncvtbf16.f.f.w
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				const float32_t a = fpu::make<float32_t>(read_velem(regs, instr.rs2, 32, i));
				write_velem(regs, instr.rd, 16, i, rounded(regs, [&] { return f32_to_bf16(a); }).v);
			});
			return;
		}

		const int nsew = sew, wsew = sew * 2;
		if (wsew > 64) return;   // no format wider than 64 bits

		for_each_active(regs, vm, vl, [&](uint64_t i) {
			switch (sub) {
			case 0x08: case 0x09: case 0x0E: case 0x0F:   // int <- float, widened (.rtz: 0E, 0F)
				write_velem(regs, instr.rd, wsew, i,
				            vec_to_int(regs, read_velem(regs, instr.rs2, nsew, i), nsew, wsew,
				                       sub == 0x08 || sub == 0x0E, sub >= 0x0E));
				break;
			case 0x0A: case 0x0B:                          // float <- int, widened
				write_velem(regs, instr.rd, wsew, i,
				            vec_from_int(regs, read_velem(regs, instr.rs2, nsew, i), nsew, wsew, sub == 0x0A));
				break;
			case 0x10: case 0x11: case 0x16: case 0x17:   // int <- float, narrowed (.rtz: 16, 17)
				write_velem(regs, instr.rd, nsew, i,
				            vec_to_int(regs, read_velem(regs, instr.rs2, wsew, i), wsew, nsew,
				                       sub == 0x10 || sub == 0x16, sub >= 0x16));
				break;
			case 0x12: case 0x13:                          // float <- int, narrowed
				write_velem(regs, instr.rd, nsew, i,
				            vec_from_int(regs, read_velem(regs, instr.rs2, wsew, i), wsew, nsew, sub == 0x12));
				break;
			case 0x0C: case 0x14: case 0x15:               // vfwcvt.f.f.v, vfncvt.f.f.w, vfncvt.rod.f.f.w
				fpu::for_width(nsew, [&](auto z) {
					using T = decltype(z);
					if constexpr (Fmt<T>::width < 64) {
						using W = typename fpu::Wider<T>::type;
						if (sub == 0x0C) {   // widening: always exact
							const T a = elem<T>(regs, instr.rs2, i);
							set_elem(regs, instr.rd, i, rounded(regs, [&] { return fpu::convert<W>(a); }));
							return;
						}
						// Narrowing. Round-to-odd (.rod) exists so that a
						// narrowing done in two steps cannot double-round;
						// SoftFloat has it as a rounding mode.
						const W a = elem<W>(regs, instr.rs2, i);
						sf::begin(7, regs.get_frm());
						if (sub == 0x15) softfloat_roundingMode = softfloat_round_odd;
						const T r = fpu::convert<T>(a);
						sf::end(regs);
						set_elem(regs, instr.rd, i, r);
					}
				});
				break;
			default: break;   // reserved
			}
		});
		return;
	}

	// VFUNARY0, same width: vfcvt.xu.f, .x.f, .f.xu, .f.x, .rtz.xu.f, .rtz.x.f.
	if (is_vv && funct6 == 0x12) {
		const uint8_t sub = instr.rs1;
		if (sub == 0x04 || sub == 0x05 || sub > 0x07) return;
		const bool to_int = sub <= 0x01 || sub >= 0x06;
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			const uint64_t raw = read_velem(regs, instr.rs2, sew, i);
			write_velem(regs, instr.rd, sew, i,
			            to_int ? vec_to_int(regs, raw, sew, sew, sub == 0x00 || sub == 0x06, sub >= 0x06)
			                   : vec_from_int(regs, raw, sew, sew, sub == 0x02));
		});
		return;
	}

	// VFUNARY1: vfsqrt.v, vfrsqrt7.v, vfrec7.v, vfclass.v (vs1 selects). The
	// two estimates are the architecture's 7-bit lookup tables, SoftFloat's
	// rsqrte7/recip7 as spike has them; vfrec7's out-of-range results round
	// by frm.
	if (is_vv && funct6 == 0x13) {
		const uint8_t sub = instr.rs1;
		fpu::for_width(sew, [&](auto z) {
			using T = decltype(z);
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				const T a = elem<T>(regs, instr.rs2, i);
				switch (sub) {
				case 0x00: set_elem(regs, instr.rd, i, rounded(regs, [&] { return fpu::sqrt(a); })); break;
				case 0x04: set_elem(regs, instr.rd, i, rounded(regs, [&] { return fpu::rsqrte7(a); })); break;
				case 0x05: set_elem(regs, instr.rd, i, rounded(regs, [&] { return fpu::recip7(a); })); break;
				case 0x10: write_velem(regs, instr.rd, sew, i, fpu::classify(a)); break;
				default: break;
				}
			});
		});
		return;
	}

	// Everything else is per-element arithmetic and compares, .vv and .vf,
	// keyed on funct6, at SEW -- or, for the widening forms, computed at 2*SEW.
	fpu::for_width(sew, [&](auto z) {
		using T = decltype(z);
		const T scalar = is_vv ? T{} : fpu::read<T>(regs, instr.rs1);
		auto vs2 = [&](uint64_t i) { return elem<T>(regs, instr.rs2, i); };
		auto op1 = [&](uint64_t i) { return is_vv ? elem<T>(regs, instr.rs1, i) : scalar; };
		auto arith = [&](auto f) {
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				set_elem(regs, instr.rd, i, rounded(regs, [&] { return f(vs2(i), op1(i)); }));
			});
		};
		auto compare = [&](auto f) {
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				set_mask_bit(regs, instr.rd, i, rounded(regs, [&] { return f(vs2(i), op1(i)); }));
			});
		};

		switch (funct6) {
		case 0x00: arith([](T a, T b) { return fpu::add(a, b); }); return;   // vfadd
		case 0x02: arith([](T a, T b) { return fpu::sub(a, b); }); return;   // vfsub
		case 0x27: arith([](T a, T b) { return fpu::sub(b, a); }); return;   // vfrsub.vf
		case 0x24: arith([](T a, T b) { return fpu::mul(a, b); }); return;   // vfmul
		case 0x20: arith([](T a, T b) { return fpu::div(a, b); }); return;   // vfdiv
		case 0x21: arith([](T a, T b) { return fpu::div(b, a); }); return;   // vfrdiv.vf
		case 0x04: arith([](T a, T b) { return fpu::min_max(a, b, false); }); return;   // vfmin
		case 0x06: arith([](T a, T b) { return fpu::min_max(a, b, true); }); return;    // vfmax
		case 0x08: case 0x09: case 0x0a:   // vfsgnj, vfsgnjn, vfsgnjx: bits, no flags
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				set_elem(regs, instr.rd, i, fpu::sign_inject(vs2(i), op1(i), funct6 - 0x08u));
			});
			return;
		case 0x18: compare([](T a, T b) { return fpu::eq(a, b); }); return;    // vmfeq
		case 0x19: compare([](T a, T b) { return fpu::le(a, b); }); return;    // vmfle
		case 0x1b: compare([](T a, T b) { return fpu::lt(a, b); }); return;    // vmflt
		case 0x1c: compare([](T a, T b) { return !fpu::eq(a, b); }); return;   // vmfne: unordered is "not equal"
		case 0x1d: compare([](T a, T b) { return fpu::lt(b, a); }); return;    // vmfgt.vf
		case 0x1f: compare([](T a, T b) { return fpu::le(b, a); }); return;    // vmfge.vf
		default: break;
		}

		if (funct6 >= 0x28 && funct6 <= 0x2f) {
			// The fused multiply-adds. The macc family (bit 2 set) adds to vd;
			// the madd family multiplies vd. Negation flips the sign bits of
			// the operands going in, so the fusion stays one rounding.
			const bool macc = (funct6 >> 2) & 1;
			const uint8_t neg = funct6 & 0x3;
			const bool negate_a = (neg == 0b01 || neg == 0b11), negate_c = (neg == 0b01 || neg == 0b10);
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				const T vd = elem<T>(regs, instr.rd, i), v2 = vs2(i), o1 = op1(i);
				T a = macc ? o1 : vd, c = macc ? vd : v2;
				const T b = macc ? v2 : o1;
				if (negate_a) a = fpu::negate(a);
				if (negate_c) c = fpu::negate(c);
				set_elem(regs, instr.rd, i, rounded(regs, [&] { return fpu::mul_add(a, b, c); }));
			});
			return;
		}

		if (funct6 >= 0x30) {
			// Widening: narrow operands converted up (exactly, with invalid
			// for a signaling NaN, as Sail and spike convert them) and one
			// rounding at the wide format.
			if constexpr (Fmt<T>::width < 64) {
				using W = typename fpu::Wider<T>::type;
				const bool vs2_wide = (funct6 == 0x34 || funct6 == 0x36);   // the .wv / .wf forms
				auto vs2w = [&](uint64_t i) {
					return vs2_wide ? elem<W>(regs, instr.rs2, i) : fpu::convert<W>(vs2(i));
				};
				auto op1w = [&](uint64_t i) { return fpu::convert<W>(op1(i)); };

				if (funct6 == 0x31 || funct6 == 0x33) {   // vfwredusum.vs / vfwredosum.vs
					W acc = elem<W>(regs, instr.rs1, 0);
					for_each_active(regs, vm, vl, [&](uint64_t i) {
						acc = rounded(regs, [&] { return fpu::add(acc, fpu::convert<W>(vs2(i))); });
					});
					if (vl > 0) set_elem(regs, instr.rd, 0, acc);
					return;
				}

				if (funct6 == 0x3b && Fmt<T>::width == 16) {   // vfwmaccbf16.vv / .vf
					// The scalar, unboxed as a bf16: a value not properly boxed
					// reads as bf16's canonical NaN, 0x7FC0, not half's. Sail
					// widens it once, before the elements, raising invalid for
					// a signaling NaN even when vl is 0; so does this.
					const uint64_t fbits = regs.read_f_bits(instr.rs1);
					const uint16_t fs = (fbits >> 16) == 0xFFFFFFFFFFFFull ? (uint16_t)fbits : 0x7FC0;
					const float32_t scalar = is_vv ? float32_t{} : rounded(regs, [&] { return bf16_to_f32(bf16(fs)); });
					for_each_active(regs, vm, vl, [&](uint64_t i) {
						const float32_t c = elem<float32_t>(regs, instr.rd, i);
						set_elem(regs, instr.rd, i, rounded(regs, [&] {
							const float32_t a = is_vv ? bf16_to_f32(bf16(read_velem(regs, instr.rs1, 16, i))) : scalar;
							const float32_t b = bf16_to_f32(bf16(read_velem(regs, instr.rs2, 16, i)));
							return fpu::mul_add(a, b, c);
						}));
					});
					return;
				}

				auto warith = [&](auto f) {
					for_each_active(regs, vm, vl, [&](uint64_t i) {
						set_elem(regs, instr.rd, i, rounded(regs, [&] { return f(vs2w(i), op1w(i)); }));
					});
				};
				switch (funct6) {
				case 0x30: case 0x34: warith([](W a, W b) { return fpu::add(a, b); }); return;   // vfwadd(.w)
				case 0x32: case 0x36: warith([](W a, W b) { return fpu::sub(a, b); }); return;   // vfwsub(.w)
				case 0x38:            warith([](W a, W b) { return fpu::mul(a, b); }); return;   // vfwmul
				case 0x3c: case 0x3d: case 0x3e: case 0x3f: {   // vfwmacc, vfwnmacc, vfwmsac, vfwnmsac
					const uint8_t neg = funct6 & 0x3;
					const bool negate_a = (neg == 0b01 || neg == 0b11), negate_c = (neg == 0b01 || neg == 0b10);
					for_each_active(regs, vm, vl, [&](uint64_t i) {
						set_elem(regs, instr.rd, i, rounded(regs, [&] {
							W a = op1w(i), c = elem<W>(regs, instr.rd, i);
							const W b = vs2w(i);
							if (negate_a) a = fpu::negate(a);
							if (negate_c) c = fpu::negate(c);
							return fpu::mul_add(a, b, c);
						}));
					});
					return;
				}
				default: return;
				}
			}
		}
	});
}

} // namespace vcommon
