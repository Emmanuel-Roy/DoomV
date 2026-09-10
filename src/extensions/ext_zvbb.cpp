// Zvbb: vector basic bit manipulation -- the whole extension.
//
// This file used to hold vandn alone, on the reasoning that it was the one
// a distro riscv64 glibc actually emits and that "guessing at
// implementations nothing exercises is how the mask-register bug got in".
// The first half of that was true; the conclusion was not. RVA23 mandates
// all of Zvbb, and the way to avoid guessing is a differential test, not an
// empty file -- an unimplemented funct6 here did not fail loudly, it fell
// through to exec_v_int and was silently ignored, which is worse than
// either.
//
// Encodings, taken from the assembler rather than from memory:
//
//   OPIVV/OPIVX  funct6 0x01       vandn.vv / vandn.vx
//   OPMVV        funct6 0x12       vbrev8(8) vrev8(9) vbrev(10)
//                                  vclz(12) vctz(13) vcpop(14) -- by vs1
//   OPIVV/OPIVX  funct6 0x14       vror.vv / vror.vx
//   OPIVI        funct6 0x14/0x15  vror.vi
//   OPIVV/OPIVX  funct6 0x15       vrol.vv / vrol.vx
//   OPIV*        funct6 0x35       vwsll.vv / .vx / .vi   (widening)
//
// Two of those overlap in ways worth stating, because both are exactly what
// a remembered opcode table gets wrong:
//
//   * funct6 0x12 is shared with base V's vzext/vsext, which use vs1 values
//     2..7. Zvbb takes 8..14 in the same slot, so the split is on vs1, not
//     on funct6.
//
//   * vror.vi needs a 6-bit rotate amount but OPIVI's immediate field is 5
//     bits, so imm[5] lives in the low bit of funct6. That is why vror.vi
//     appears at both 0x14 and 0x15, and why 0x15 means vrol for
//     OPIVV/OPIVX but vror for OPIVI -- funct3 is what separates them.
//
// Not gated separately from V: a userspace with vandn.vv in its string
// routines has all of Zvbb unconditionally, so a separate toggle would only
// add another way to configure a guest into failing.
//
// The operand plumbing is re-derived here rather than shared with
// exec_v_int -- the same convention the rest of the vector files follow,
// each deriving vtype/vl/SEW and its own operand selection.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"

using namespace vcommon;

namespace {

// Bit-reverse the low `bits` bits of v.
uint64_t brev(uint64_t v, int bits)
{
	uint64_t r = 0;
	for (int i = 0; i < bits; i++)
		if (v & (1ull << i)) r |= 1ull << (bits - 1 - i);
	return r;
}

// Reverse the bits within each byte, leaving byte order alone (vbrev8).
uint64_t brev8(uint64_t v, int bytes)
{
	uint64_t r = 0;
	for (int b = 0; b < bytes; b++)
		r |= brev((v >> (8 * b)) & 0xFF, 8) << (8 * b);
	return r;
}

// Reverse byte order within the element, leaving each byte alone (vrev8).
uint64_t rev8(uint64_t v, int bytes)
{
	uint64_t r = 0;
	for (int b = 0; b < bytes; b++)
		r |= ((v >> (8 * b)) & 0xFF) << (8 * (bytes - 1 - b));
	return r;
}

// Counting leading/trailing zeros is defined over the *element* width, not
// 64 bits, so an all-zero element yields SEW rather than 64. A host
// __builtin_clzll here would return the wrong answer for every SEW < 64.
uint64_t clz_sew(uint64_t v, int bits)
{
	for (int i = bits - 1; i >= 0; i--)
		if (v & (1ull << i)) return (uint64_t)(bits - 1 - i);
	return (uint64_t)bits;
}

uint64_t ctz_sew(uint64_t v, int bits)
{
	for (int i = 0; i < bits; i++)
		if (v & (1ull << i)) return (uint64_t)i;
	return (uint64_t)bits;
}

uint64_t cpop_sew(uint64_t v, int bits)
{
	uint64_t n = 0;
	for (int i = 0; i < bits; i++)
		if (v & (1ull << i)) n++;
	return n;
}

// Rotates are modulo the element width and have to happen inside that
// width: a 64-bit host rotate of an 8-bit element would drag in the zeros
// above bit 7. Only log2(SEW) bits of the amount are used, per spec.
uint64_t rotr_sew(uint64_t v, uint64_t amt, int bits, uint64_t smask)
{
	amt &= (uint64_t)(bits - 1);
	if (amt == 0) return v & smask;
	return ((v >> amt) | (v << (bits - amt))) & smask;
}

uint64_t rotl_sew(uint64_t v, uint64_t amt, int bits, uint64_t smask)
{
	amt &= (uint64_t)(bits - 1);
	if (amt == 0) return v & smask;
	return ((v << amt) | (v >> (bits - amt))) & smask;
}

} // namespace

namespace vcommon {

// The OPMVV funct6=0x12 unary group. Split out because its operand shape
// differs from everything else here: there is no second operand at all, and
// vs1 is an opcode extension rather than a register number.
void exec_zvbb_unary(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	bool vm = op_v_vm(instr.funct7);
	uint64_t smask = elem_mask(sew);
	int bytes = sew / 8;

	for_each_active(regs, vm, vl, [&](uint64_t i) {
		uint64_t a = read_velem(regs, instr.rs2, sew, i);
		uint64_t r = 0;
		switch (instr.rs1) {
		case 8:  r = brev8(a, bytes);  break; // vbrev8.v
		case 9:  r = rev8(a, bytes);   break; // vrev8.v
		case 10: r = brev(a, sew);     break; // vbrev.v
		case 12: r = clz_sew(a, sew);  break; // vclz.v
		case 13: r = ctz_sew(a, sew);  break; // vctz.v
		case 14: r = cpop_sew(a, sew); break; // vcpop.v
		}
		write_velem(regs, instr.rd, sew, i, r & smask);
	});
}

void exec_zvbb(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	int sew = vt.sew;
	uint64_t vl = regs.get_vl();
	bool vm = op_v_vm(instr.funct7);
	uint8_t funct6 = op_v_funct6(instr.funct7);
	uint64_t smask = elem_mask(sew);

	bool is_vv = (instr.funct3 == 0b000);
	bool is_vi = (instr.funct3 == 0b011);

	// vwsll doubles the element width, so its destination is written at
	// 2*SEW while its sources stay at SEW.
	if (funct6 == 0x35) {
		int wsew = sew * 2;
		uint64_t wmask = elem_mask(wsew);
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			uint64_t a = read_velem(regs, instr.rs2, sew, i);
			// The shift amount is taken modulo the *widened* width --
			// this is a widening shift, so the result has room for it.
			uint64_t amt = is_vv ? read_velem(regs, instr.rs1, sew, i)
			             : is_vi ? (uint64_t)instr.rs1
			                     : regs.read_x(instr.rs1);
			amt &= (uint64_t)(wsew - 1);
			write_velem(regs, instr.rd, wsew, i, (a << amt) & wmask);
		});
		return;
	}

	// The second operand: a vector element (.vv), an x-register truncated to
	// SEW (.vx), or the immediate (.vi). For vror.vi that immediate is 6
	// bits -- vs1 supplies the low 5, funct6's low bit the sixth -- and it
	// is zero-extended, not sign-extended like base V's simm5 forms.
	auto op2 = [&](uint64_t i) -> uint64_t {
		if (is_vv) return read_velem(regs, instr.rs1, sew, i);
		if (is_vi) return (uint64_t)instr.rs1 | ((uint64_t)(funct6 & 1) << 5);
		return regs.read_x(instr.rs1) & smask;
	};

	switch (funct6) {
	case 0x01: // vandn: vd = vs2 & ~op2
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i,
			            (read_velem(regs, instr.rs2, sew, i) & ~op2(i)) & smask);
		});
		break;

	case 0x14: // vror.vv/.vx, and vror.vi with imm[5] == 0
		for_each_active(regs, vm, vl, [&](uint64_t i) {
			write_velem(regs, instr.rd, sew, i,
			            rotr_sew(read_velem(regs, instr.rs2, sew, i), op2(i), sew, smask));
		});
		break;

	case 0x15: // vrol.vv/.vx -- but vror.vi with imm[5] == 1 lands here too
		if (is_vi) {
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, sew, i,
				            rotr_sew(read_velem(regs, instr.rs2, sew, i), op2(i), sew, smask));
			});
		} else {
			for_each_active(regs, vm, vl, [&](uint64_t i) {
				write_velem(regs, instr.rd, sew, i,
				            rotl_sew(read_velem(regs, instr.rs2, sew, i), op2(i), sew, smask));
			});
		}
		break;
	}
}

// Zvbc: vector carry-less multiply.
//
// The scalar Zbc pair widened to a vector, and defined only for SEW=64 --
// which is the point of it. A 64x64 carry-less multiply produces a 128-bit
// product, and GHASH, the authenticator in AES-GCM, is a chain of exactly
// those over a 128-bit field. Doing them an element at a time is what makes
// authenticated encryption fast; the scalar forms exist for CRCs, where one
// at a time is all anyone needs.
//
//   OPMVV/OPMVX funct6 0x0C   vclmul.vv  / vclmul.vx    low 64 bits
//   OPMVV/OPMVX funct6 0x0D   vclmulh.vv / vclmulh.vx   high 64 bits
//
// Both funct6 values are unassigned in the base vector ISA, so nothing needs
// disambiguating -- unlike Zvbb's unary group, which lives inside base V's
// vzext/vsext slot and is separated by vs1.
namespace {

// Multiplication in GF(2): partial products are XORed rather than added, so
// no carry propagates. The high half collects the bits that fall off the
// top, and i == 0 is excluded because a >> 64 is undefined rather than zero.
uint64_t v_clmul_lo(uint64_t a, uint64_t b)
{
	uint64_t r = 0;
	for (int i = 0; i < 64; i++)
		if ((b >> i) & 1ull) r ^= a << i;
	return r;
}

uint64_t v_clmul_hi(uint64_t a, uint64_t b)
{
	uint64_t r = 0;
	for (int i = 1; i < 64; i++)
		if ((b >> i) & 1ull) r ^= a >> (64 - i);
	return r;
}

} // namespace

void exec_zvbc(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	// SEW=64 only. Every other width is reserved, and the permissive stance
	// the rest of this decoder takes for a reserved encoding is to retire
	// without effect rather than trap.
	if (vt.sew != 64) return;

	const uint64_t vl = regs.get_vl();
	const bool vm = op_v_vm(instr.funct7);
	const bool is_vv = (instr.funct3 == 0b010);
	const bool high = (op_v_funct6(instr.funct7) == 0x0d);
	const uint64_t xs = regs.read_x(instr.rs1);

	for_each_active(regs, vm, vl, [&](uint64_t i) {
		const uint64_t a = read_velem(regs, instr.rs2, 64, i);
		const uint64_t b = is_vv ? read_velem(regs, instr.rs1, 64, i) : xs;
		write_velem(regs, instr.rd, 64, i,
		            high ? v_clmul_hi(a, b) : v_clmul_lo(a, b));
	});
}

} // namespace vcommon
