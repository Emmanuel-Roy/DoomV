// Zvkned: vector AES, and Zvkg's GCM multiply alongside it.
//
// These live in their own major opcode, OP-VE (0x77), not in OP-V with the
// rest of the vector ISA -- which is why the whole family decoded as an
// illegal instruction here rather than as a wrong operation. Everything in
// this opcode is funct3=010 and selected by funct6 plus the vs1 field:
//
//   funct6 0x28  vs1 = 0 vaesdm.vv   1 vaesdf.vv   2 vaesem.vv   3 vaesef.vv
//                vs1 = 17 vgmul.vv                         (Zvkg)
//   funct6 0x29  vs1 = 0 vaesdm.vs   1 vaesdf.vs   2 vaesem.vs   3 vaesef.vs
//                vs1 = 7  vaesz.vs
//   funct6 0x22  vaeskf1.vi   (vs1 is the round number, not a register)
//   funct6 0x2a  vaeskf2.vi   (likewise)
//   funct6 0x2c  vghsh.vv                                  (Zvkg)
//
// The .vv and .vs forms differ only in where the round key comes from: .vv
// takes a fresh key from the matching element group of vs2, .vs takes group
// zero for every group. That is the difference between encrypting many
// blocks under many keys and many blocks under one, and the second is the
// common case.
//
// All of it works on 128-bit element groups -- four consecutive SEW=32
// elements, EGW=128 in the spec's terms -- so the loop counts groups rather
// than elements, and vl and vstart are divided by four.
//
// The AES tables and the exact round structure are transcribed from the Sail
// model rather than written from memory. Sail's vectors are descending-index
// and its lookups read table[255 - x], so the literals as listed go straight
// into an ascending C array: sbox[0] is 0x63, which is the standard S-box.
// Getting that backwards is a real hazard -- it cost a round of debugging on
// the vfrec7 tables in this same file's neighbour.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>

namespace vcommon {
namespace {

const uint8_t SBOX_FWD[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

// The inverse S-box is derived rather than transcribed: it is by definition
// the permutation that undoes the forward one, so building it from the table
// above cannot disagree with it. Two hand-copied 256-entry tables can.
struct InvSbox {
	uint8_t t[256];
	InvSbox() { for (int i = 0; i < 256; i++) t[SBOX_FWD[i]] = (uint8_t)i; }
};
const InvSbox SBOX_INV;

// Multiply by x in GF(2^8) modulo the AES polynomial: shift left, and fold
// in 0x1b when a bit falls off the top.
inline uint8_t xt2(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }
inline uint8_t xt3(uint8_t x) { return (uint8_t)(x ^ xt2(x)); }

// Multiply an 8-bit field element by a 4-bit constant, which is all
// MixColumns ever needs.
inline uint8_t gfmul(uint8_t x, uint8_t y)
{
	uint8_t r = 0, p = x;
	for (int i = 0; i < 4; i++) {
		if ((y >> i) & 1) r ^= p;
		p = xt2(p);
	}
	return r;
}

inline uint8_t byte_of(uint32_t w, int i) { return (uint8_t)(w >> (8 * i)); }

inline uint32_t subword_fwd(uint32_t w)
{
	return ((uint32_t)SBOX_FWD[byte_of(w, 3)] << 24)
	     | ((uint32_t)SBOX_FWD[byte_of(w, 2)] << 16)
	     | ((uint32_t)SBOX_FWD[byte_of(w, 1)] << 8)
	     | (uint32_t)SBOX_FWD[byte_of(w, 0)];
}

inline uint32_t subword_inv(uint32_t w)
{
	return ((uint32_t)SBOX_INV.t[byte_of(w, 3)] << 24)
	     | ((uint32_t)SBOX_INV.t[byte_of(w, 2)] << 16)
	     | ((uint32_t)SBOX_INV.t[byte_of(w, 1)] << 8)
	     | (uint32_t)SBOX_INV.t[byte_of(w, 0)];
}

inline uint32_t mixcolumn_fwd(uint32_t x)
{
	const uint8_t s0 = byte_of(x, 0), s1 = byte_of(x, 1);
	const uint8_t s2 = byte_of(x, 2), s3 = byte_of(x, 3);
	const uint8_t b0 = (uint8_t)(xt2(s0) ^ xt3(s1) ^ s2 ^ s3);
	const uint8_t b1 = (uint8_t)(s0 ^ xt2(s1) ^ xt3(s2) ^ s3);
	const uint8_t b2 = (uint8_t)(s0 ^ s1 ^ xt2(s2) ^ xt3(s3));
	const uint8_t b3 = (uint8_t)(xt3(s0) ^ s1 ^ s2 ^ xt2(s3));
	return ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;
}

inline uint32_t mixcolumn_inv(uint32_t x)
{
	const uint8_t s0 = byte_of(x, 0), s1 = byte_of(x, 1);
	const uint8_t s2 = byte_of(x, 2), s3 = byte_of(x, 3);
	const uint8_t b0 = (uint8_t)(gfmul(s0,0xE) ^ gfmul(s1,0xB) ^ gfmul(s2,0xD) ^ gfmul(s3,0x9));
	const uint8_t b1 = (uint8_t)(gfmul(s0,0x9) ^ gfmul(s1,0xE) ^ gfmul(s2,0xB) ^ gfmul(s3,0xD));
	const uint8_t b2 = (uint8_t)(gfmul(s0,0xD) ^ gfmul(s1,0x9) ^ gfmul(s2,0xE) ^ gfmul(s3,0xB));
	const uint8_t b3 = (uint8_t)(gfmul(s0,0xB) ^ gfmul(s1,0xD) ^ gfmul(s2,0x9) ^ gfmul(s3,0xE));
	return ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;
}

// A 128-bit AES state as four columns, column 0 in the low 32 bits -- which
// is exactly how a 128-bit element group sits in the vector register.
struct Blk { uint32_t c[4]; };

inline void shift_rows_fwd(Blk &b)
{
	const uint32_t i0 = b.c[0], i1 = b.c[1], i2 = b.c[2], i3 = b.c[3];
	b.c[0] = (i3 & 0xFF000000u) | (i2 & 0x00FF0000u) | (i1 & 0x0000FF00u) | (i0 & 0xFFu);
	b.c[1] = (i0 & 0xFF000000u) | (i3 & 0x00FF0000u) | (i2 & 0x0000FF00u) | (i1 & 0xFFu);
	b.c[2] = (i1 & 0xFF000000u) | (i0 & 0x00FF0000u) | (i3 & 0x0000FF00u) | (i2 & 0xFFu);
	b.c[3] = (i2 & 0xFF000000u) | (i1 & 0x00FF0000u) | (i0 & 0x0000FF00u) | (i3 & 0xFFu);
}

inline void shift_rows_inv(Blk &b)
{
	const uint32_t i0 = b.c[0], i1 = b.c[1], i2 = b.c[2], i3 = b.c[3];
	b.c[0] = (i1 & 0xFF000000u) | (i2 & 0x00FF0000u) | (i3 & 0x0000FF00u) | (i0 & 0xFFu);
	b.c[1] = (i2 & 0xFF000000u) | (i3 & 0x00FF0000u) | (i0 & 0x0000FF00u) | (i1 & 0xFFu);
	b.c[2] = (i3 & 0xFF000000u) | (i0 & 0x00FF0000u) | (i1 & 0x0000FF00u) | (i2 & 0xFFu);
	b.c[3] = (i0 & 0xFF000000u) | (i1 & 0x00FF0000u) | (i2 & 0x0000FF00u) | (i3 & 0xFFu);
}

inline uint32_t rcon(unsigned r)
{
	static const uint32_t T[10] = {1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
	return (r < 10) ? T[r] : 0;   // r >= 10 is unreachable once rnd is clamped
}

inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

inline Blk read_grp(Registers &regs, int vreg, uint64_t g)
{
	Blk b;
	for (int k = 0; k < 4; k++)
		b.c[k] = (uint32_t)read_velem(regs, vreg, 32, g * 4 + k);
	return b;
}

inline void write_grp(Registers &regs, int vreg, uint64_t g, const Blk &b)
{
	for (int k = 0; k < 4; k++)
		write_velem(regs, vreg, 32, g * 4 + k, b.c[k]);
}

// GCM's field multiply, for Zvkg. The GHASH field is GF(2^128) reduced by
// x^128 + x^7 + x^2 + x + 1 -- the 0x87 folded in below -- but GCM stores
// its field elements bit-reversed within each byte relative to the natural
// little-endian order a vector register holds. So brev8 goes in on both
// operands and comes back off the product; between those the multiply is
// the ordinary shift-and-xor, walking the multiplier's bits from bit 0 up
// while the multiplicand shifts left.
//
// This is worth spelling out because the obvious implementation -- walk
// down from bit 127 and shift right -- is what you get from thinking about
// GHASH as it is usually *described*, and it is a different function.
struct U128 { uint64_t lo, hi; };

inline U128 to_u128(const Blk &b)
{
	return U128{ (uint64_t)b.c[0] | ((uint64_t)b.c[1] << 32),
	             (uint64_t)b.c[2] | ((uint64_t)b.c[3] << 32) };
}

inline Blk from_u128(const U128 &v)
{
	Blk b;
	b.c[0] = (uint32_t)v.lo; b.c[1] = (uint32_t)(v.lo >> 32);
	b.c[2] = (uint32_t)v.hi; b.c[3] = (uint32_t)(v.hi >> 32);
	return b;
}

inline uint8_t brev8_byte(uint8_t x)
{
	x = (uint8_t)(((x & 0xF0) >> 4) | ((x & 0x0F) << 4));
	x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
	x = (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
	return x;
}

inline uint64_t brev8_64(uint64_t v)
{
	uint64_t r = 0;
	for (int i = 0; i < 8; i++)
		r |= (uint64_t)brev8_byte((uint8_t)(v >> (8 * i))) << (8 * i);
	return r;
}

inline U128 brev8_128(const U128 &v) { return U128{ brev8_64(v.lo), brev8_64(v.hi) }; }

U128 ghash_mul(const U128 &s_in, const U128 &h_in)
{
	uint64_t zlo = 0, zhi = 0, hlo = h_in.lo, hhi = h_in.hi;
	for (int b = 0; b < 128; b++) {
		const uint64_t bit = (b < 64) ? ((s_in.lo >> b) & 1ull)
		                             : ((s_in.hi >> (b - 64)) & 1ull);
		if (bit) { zlo ^= hlo; zhi ^= hhi; }
		const bool reduce = (hhi >> 63) & 1;
		hhi = (hhi << 1) | (hlo >> 63);
		hlo <<= 1;
		if (reduce) hlo ^= 0x87;
	}
	return U128{ zlo, zhi };
}

} // namespace

void exec_zvkned(const DecodedInstruction &instr, Registers &regs)
{
	VType vt = decode_vtype(regs.get_vtype());
	// Every instruction here is defined for SEW=32 only: the element group
	// is 128 bits wide and made of four of them.
	if (vt.sew != 32) return;

	// A vl or vstart that is not a multiple of the group size, and an
	// LMUL*VLEN smaller than one group, are reserved -- Sail raises an
	// illegal instruction for both. Here the integer division below simply
	// drops the partial group, which is the permissive-illegal stance the
	// rest of this decoder takes for a reserved vector encoding. It is a
	// known divergence from the reference rather than an oversight; nothing
	// in riscv-vector-tests reaches it, so it is untested either way. The
	// same applies in ext_zvknh.cpp and ext_zvksm.cpp.

	const uint64_t vl = regs.get_vl();
	const uint8_t funct6 = op_v_funct6(instr.funct7);
	const uint8_t sel = instr.rs1;      // operation selector, or the round
	const uint64_t groups = vl / 4;
	const uint64_t start = regs.get_vstart() / 4;

	for (uint64_t g = start; g < groups; g++) {
		// .vs forms take the round key from group zero for every block;
		// .vv forms take a fresh one per block.
		const bool vs_form = (funct6 == 0x29);
		Blk state = read_grp(regs, instr.rd, g);

		if (funct6 == 0x22 || funct6 == 0x2a) {
			// Key schedule. The round number is an immediate in the vs1
			// field, and out-of-range values are folded back in rather than
			// trapping -- the spec flips bit 3, which is what these lines
			// do, so that every encoding decodes to something defined.
			const Blk key = read_grp(regs, instr.rs2, g);
			Blk w;
			if (funct6 == 0x22) {          // vaeskf1.vi
				unsigned rn = sel & 0x1F;
				if ((rn & 0xF) > 10 || (rn & 0xF) == 0) rn ^= 0x8;
				const unsigned r = ((rn & 0xF) - 1) & 0xF;
				w.c[0] = subword_fwd(rotr32(key.c[3], 8)) ^ rcon(r) ^ key.c[0];
			} else {                        // vaeskf2.vi
				// Four bits, not five: the clamp and the rcon index are
				// both taken from rnd[3:0]. Reading five bits sends
				// rnd=0x10 to 24, and (24>>1)-1 = 11 indexes past the
				// ten-entry rcon table.
				unsigned rn = sel & 0xF;
				if (rn < 2 || rn > 14) rn ^= 0x8;
				if (rn & 1)
					w.c[0] = subword_fwd(key.c[3]) ^ state.c[0];
				else
					w.c[0] = subword_fwd(rotr32(key.c[3], 8))
					       ^ rcon(((rn >> 1) - 1) & 0xF) ^ state.c[0];
			}
			const Blk &base = (funct6 == 0x22) ? key : state;
			w.c[1] = w.c[0] ^ base.c[1];
			w.c[2] = w.c[1] ^ base.c[2];
			w.c[3] = w.c[2] ^ base.c[3];
			write_grp(regs, instr.rd, g, w);
			continue;
		}

		if (funct6 == 0x2c) {              // vghsh.vv
			// Y ^= X first, then into the bit-reversed domain. vgmul below
			// is the same thing with no X to absorb.
			Blk y = state;
			const Blk xi = read_grp(regs, instr.rs1, g);
			for (int k = 0; k < 4; k++) y.c[k] ^= xi.c[k];
			const U128 h = brev8_128(to_u128(read_grp(regs, instr.rs2, g)));
			const U128 z = ghash_mul(brev8_128(to_u128(y)), h);
			write_grp(regs, instr.rd, g, from_u128(brev8_128(z)));
			continue;
		}
		if (funct6 == 0x28 && sel == 17) { // vgmul.vv
			const U128 h = brev8_128(to_u128(read_grp(regs, instr.rs2, g)));
			const U128 z = ghash_mul(brev8_128(to_u128(state)), h);
			write_grp(regs, instr.rd, g, from_u128(brev8_128(z)));
			continue;
		}

		const Blk rkey = read_grp(regs, instr.rs2, vs_form ? 0 : g);

		if (vs_form && sel == 7) {         // vaesz.vs -- the round-zero key XOR
			for (int k = 0; k < 4; k++) state.c[k] ^= rkey.c[k];
			write_grp(regs, instr.rd, g, state);
			continue;
		}

		switch (sel) {
		case 0: {                          // vaesdm -- decrypt middle round
			shift_rows_inv(state);
			for (int k = 0; k < 4; k++) state.c[k] = subword_inv(state.c[k]);
			for (int k = 0; k < 4; k++) state.c[k] ^= rkey.c[k];
			for (int k = 0; k < 4; k++) state.c[k] = mixcolumn_inv(state.c[k]);
			break;
		}
		case 1: {                          // vaesdf -- decrypt final round
			shift_rows_inv(state);
			for (int k = 0; k < 4; k++) state.c[k] = subword_inv(state.c[k]);
			for (int k = 0; k < 4; k++) state.c[k] ^= rkey.c[k];
			break;
		}
		case 2: {                          // vaesem -- encrypt middle round
			for (int k = 0; k < 4; k++) state.c[k] = subword_fwd(state.c[k]);
			shift_rows_fwd(state);
			for (int k = 0; k < 4; k++) state.c[k] = mixcolumn_fwd(state.c[k]);
			for (int k = 0; k < 4; k++) state.c[k] ^= rkey.c[k];
			break;
		}
		case 3: {                          // vaesef -- encrypt final round
			for (int k = 0; k < 4; k++) state.c[k] = subword_fwd(state.c[k]);
			shift_rows_fwd(state);
			for (int k = 0; k < 4; k++) state.c[k] ^= rkey.c[k];
			break;
		}
		default:
			return;                        // nothing else is defined here
		}
		write_grp(regs, instr.rd, g, state);
	}

	regs.set_vstart(0);
}

} // namespace vcommon
