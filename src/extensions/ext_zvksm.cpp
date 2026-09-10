// Zvksed and Zvksh: vector SM4 and SM3, China's national block cipher and
// hash. Both live in OP-VE (0x77) with funct3=010 next to AES and SHA-2:
//
//   funct6 0x21             vsm4k.vi    SM4 key expansion, four round keys
//   funct6 0x28, vs1 = 16   vsm4r.vv    SM4 round, per-group round keys
//   funct6 0x29, vs1 = 16   vsm4r.vs    SM4 round, one key for every group
//   funct6 0x20             vsm3me.vv   SM3 message expansion, eight words
//   funct6 0x2b             vsm3c.vi    SM3 compression, two rounds
//
// SM4's element group is four 32-bit words, like AES. SM3's is *eight* --
// its state is A..H, all eight of which move every round, unlike SHA-2
// where two rounds leave half the state alone. So vl and vstart divide by
// eight there, not four.
//
// The one thing worth watching in SM3 is endianness: the algorithm is
// specified big-endian, and both its instructions byte-swap every word on
// the way in and on the way out. That keeps the words in memory order in
// the register file, so a caller can load a message block with a plain
// vector load. Miss the swap and every result is wrong while still looking
// plausibly random.
//
// All transcribed from Sail's zvksed_insts.sail, zvksh_insts.sail and
// zvk_utils.sail.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>

namespace vcommon {
namespace {

// SM4's single S-box -- one table serves encryption and decryption, since
// SM4's rounds are a Feistel structure and never need an inverse.
// Sail looks this up as table[255 - x] over a descending vector, which is
// the same as sbox[x] over this ascending array.
const uint8_t SM4_SBOX[256] = {
	0xd6,0x90,0xe9,0xfe,0xcc,0xe1,0x3d,0xb7,0x16,0xb6,0x14,0xc2,0x28,0xfb,0x2c,0x05,
	0x2b,0x67,0x9a,0x76,0x2a,0xbe,0x04,0xc3,0xaa,0x44,0x13,0x26,0x49,0x86,0x06,0x99,
	0x9c,0x42,0x50,0xf4,0x91,0xef,0x98,0x7a,0x33,0x54,0x0b,0x43,0xed,0xcf,0xac,0x62,
	0xe4,0xb3,0x1c,0xa9,0xc9,0x08,0xe8,0x95,0x80,0xdf,0x94,0xfa,0x75,0x8f,0x3f,0xa6,
	0x47,0x07,0xa7,0xfc,0xf3,0x73,0x17,0xba,0x83,0x59,0x3c,0x19,0xe6,0x85,0x4f,0xa8,
	0x68,0x6b,0x81,0xb2,0x71,0x64,0xda,0x8b,0xf8,0xeb,0x0f,0x4b,0x70,0x56,0x9d,0x35,
	0x1e,0x24,0x0e,0x5e,0x63,0x58,0xd1,0xa2,0x25,0x22,0x7c,0x3b,0x01,0x21,0x78,0x87,
	0xd4,0x00,0x46,0x57,0x9f,0xd3,0x27,0x52,0x4c,0x36,0x02,0xe7,0xa0,0xc4,0xc8,0x9e,
	0xea,0xbf,0x8a,0xd2,0x40,0xc7,0x38,0xb5,0xa3,0xf7,0xf2,0xce,0xf9,0x61,0x15,0xa1,
	0xe0,0xae,0x5d,0xa4,0x9b,0x34,0x1a,0x55,0xad,0x93,0x32,0x30,0xf5,0x8c,0xb1,0xe3,
	0x1d,0xf6,0xe2,0x2e,0x82,0x66,0xca,0x60,0xc0,0x29,0x23,0xab,0x0d,0x53,0x4e,0x6f,
	0xd5,0xdb,0x37,0x45,0xde,0xfd,0x8e,0x2f,0x03,0xff,0x6a,0x72,0x6d,0x6c,0x5b,0x51,
	0x8d,0x1b,0xaf,0x92,0xbb,0xdd,0xbc,0x7f,0x11,0xd9,0x5c,0x41,0x1f,0x10,0x5a,0xd8,
	0x0a,0xc1,0x31,0x88,0xa5,0xcd,0x7b,0xbd,0x2d,0x74,0xd0,0x12,0xb8,0xe5,0xb4,0xb0,
	0x89,0x69,0x97,0x4a,0x0c,0x96,0x77,0x7e,0x65,0xb9,0xf1,0x09,0xc5,0x6e,0xc6,0x84,
	0x18,0xf0,0x7d,0xec,0x3a,0xdc,0x4d,0x20,0x79,0xee,0x5f,0x3e,0xd7,0xcb,0x39,0x48,
};

// The family key constants CK[0..31]. The Sail helper that reads this calls
// it an sbox, which it is not; it is the key-schedule constant table.
const uint32_t SM4_CK[32] = {
	0x00070E15, 0x1C232A31, 0x383F464D, 0x545B6269,
	0x70777E85, 0x8C939AA1, 0xA8AFB6BD, 0xC4CBD2D9,
	0xE0E7EEF5, 0xFC030A11, 0x181F262D, 0x343B4249,
	0x50575E65, 0x6C737A81, 0x888F969D, 0xA4ABB2B9,
	0xC0C7CED5, 0xDCE3EAF1, 0xF8FF060D, 0x141B2229,
	0x30373E45, 0x4C535A61, 0x686F767D, 0x848B9299,
	0xA0A7AEB5, 0xBCC3CAD1, 0xD8DFE6ED, 0xF4FB0209,
	0x10171E25, 0x2C333A41, 0x484F565D, 0x646B7279,
};

inline uint32_t rotl32(uint32_t x, unsigned n)
{
	n &= 31;
	return n ? ((x << n) | (x >> (32 - n))) : x;
}

inline uint32_t sm4_subword(uint32_t x)
{
	return ((uint32_t)SM4_SBOX[(x >> 24) & 0xFF] << 24)
	     | ((uint32_t)SM4_SBOX[(x >> 16) & 0xFF] << 16)
	     | ((uint32_t)SM4_SBOX[(x >> 8) & 0xFF] << 8)
	     | (uint32_t)SM4_SBOX[x & 0xFF];
}

// The two linear diffusion layers, which are not the same: the key
// schedule's uses two rotates, the round function's four. Using either for
// the other produces something that still looks like a cipher.
inline uint32_t sm4_key_l(uint32_t x, uint32_t s)
{
	return x ^ (s ^ rotl32(s, 13) ^ rotl32(s, 23));
}

inline uint32_t sm4_round_l(uint32_t x, uint32_t s)
{
	return x ^ (s ^ rotl32(s, 2) ^ rotl32(s, 10) ^ rotl32(s, 18) ^ rotl32(s, 24));
}

// SM3's permutations, and its two round functions -- each of which changes
// form after round 15.
inline uint32_t p0(uint32_t x) { return x ^ rotl32(x, 9) ^ rotl32(x, 17); }
inline uint32_t p1(uint32_t x) { return x ^ rotl32(x, 15) ^ rotl32(x, 23); }

inline uint32_t sh_w(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
	return p1(a ^ b ^ rotl32(c, 15)) ^ rotl32(d, 7) ^ e;
}

inline uint32_t ff_j(uint32_t x, uint32_t y, uint32_t z, unsigned j)
{
	return (j <= 15) ? (x ^ y ^ z) : ((x & y) | (x & z) | (y & z));
}

inline uint32_t gg_j(uint32_t x, uint32_t y, uint32_t z, unsigned j)
{
	return (j <= 15) ? (x ^ y ^ z) : ((x & y) | (~x & z));
}

inline uint32_t t_j(unsigned j) { return (j <= 15) ? 0x79CC4519u : 0x7A879D8Au; }

inline uint32_t rev8_32(uint32_t x)
{
	return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8)
	     | ((x >> 8) & 0xFF00u) | ((x >> 24) & 0xFFu);
}

// One SM3 round over the state {A..H} held in index order, rotated in place.
void sm3_round(uint32_t s[8], uint32_t w, uint32_t x, unsigned j)
{
	const uint32_t a12 = rotl32(s[0], 12);
	const uint32_t ss1 = rotl32(a12 + s[4] + rotl32(t_j(j), j % 32), 7);
	const uint32_t ss2 = ss1 ^ a12;
	const uint32_t tt1 = ff_j(s[0], s[1], s[2], j) + s[3] + ss2 + x;
	const uint32_t tt2 = gg_j(s[4], s[5], s[6], j) + s[7] + ss1 + w;

	const uint32_t A = tt1;
	const uint32_t B = s[0];
	const uint32_t C = rotl32(s[1], 9);
	const uint32_t D = s[2];
	const uint32_t E = p0(tt2);
	const uint32_t F = s[4];
	const uint32_t G = rotl32(s[5], 19);
	const uint32_t H = s[6];
	s[0] = A; s[1] = B; s[2] = C; s[3] = D;
	s[4] = E; s[5] = F; s[6] = G; s[7] = H;
}

inline uint32_t eg32(const Registers &regs, int base, uint64_t g, int n, int k)
{
	return (uint32_t)read_velem(regs, base, 32, g * (uint64_t)n + (uint64_t)k);
}

} // namespace

void exec_zvksm(const DecodedInstruction &instr, Registers &regs)
{
	const VType vt = decode_vtype(regs.get_vtype());
	if (vt.sew != 32) return;   // both extensions are SEW=32 only

	const uint8_t funct6 = op_v_funct6(instr.funct7);
	const uint64_t vl = regs.get_vl();
	const uint64_t vstart = regs.get_vstart();

	if (funct6 == 0x20 || funct6 == 0x2b) { // SM3: eight-element groups
		const uint64_t groups = vl / 8;
		const uint64_t start = vstart / 8;

		for (uint64_t i = start; i < groups; i++) {
			if (funct6 == 0x20) { // vsm3me.vv -- eight new message words
				uint32_t w[24];
				for (int j = 0; j < 8; j++) {
					w[j] = rev8_32(eg32(regs, instr.rs1, i, 8, j));
					w[j + 8] = rev8_32(eg32(regs, instr.rs2, i, 8, j));
				}
				// Each word feeds the ones after it, so this has to run in
				// order rather than as eight independent expressions.
				for (int j = 16; j < 24; j++)
					w[j] = sh_w(w[j - 16], w[j - 9], w[j - 3], w[j - 13], w[j - 6]);
				for (int j = 0; j < 8; j++)
					write_velem(regs, instr.rd, 32, i * 8 + j, rev8_32(w[16 + j]));
				continue;
			}

			// vsm3c.vi -- two compression rounds, the pair selected by the
			// 5-bit immediate sitting in the vs1 field.
			const unsigned rnds = instr.rs1 & 0x1F;
			uint32_t s[8], w[8];
			for (int j = 0; j < 8; j++) {
				s[j] = rev8_32(eg32(regs, instr.rd, i, 8, j));
				w[j] = rev8_32(eg32(regs, instr.rs2, i, 8, j));
			}
			sm3_round(s, w[0], w[0] ^ w[4], 2 * rnds);
			// The state after the first round is written out interleaved
			// with the state after the second, so both have to be kept.
			uint32_t s1[8];
			for (int j = 0; j < 8; j++) s1[j] = s[j];
			sm3_round(s, w[1], w[1] ^ w[5], 2 * rnds + 1);

			// Even elements carry the second round's state, odd elements the
			// first's: {A2,A1,C2,C1,E2,E1,G2,G1} from element 0 up. It reads
			// as an odd layout until you notice it is what lets the next
			// vsm3c read its input straight back out.
			const uint32_t out[8] = { s[0], s1[0], s[2], s1[2],
			                          s[4], s1[4], s[6], s1[6] };
			for (int j = 0; j < 8; j++)
				write_velem(regs, instr.rd, 32, i * 8 + j, rev8_32(out[j]));
		}
		regs.set_vstart(0);
		return;
	}

	// SM4: four-element groups.
	const uint64_t groups = vl / 4;
	const uint64_t start = vstart / 4;

	for (uint64_t i = start; i < groups; i++) {
		if (funct6 == 0x21) { // vsm4k.vi -- four round keys from four inputs
			const unsigned rnd = instr.rs1 & 0x1F;
			uint32_t rk[4], out[4];
			for (int k = 0; k < 4; k++) rk[k] = eg32(regs, instr.rs2, i, 4, k);
			for (int k = 0; k < 4; k++) {
				// Each key mixes the inputs it has not consumed yet with the
				// keys already produced -- a rolling window, which is why
				// this cannot be four independent expressions.
				const uint32_t b = ((k < 1) ? rk[1] : out[0])
				                 ^ ((k < 2) ? rk[2] : out[1])
				                 ^ ((k < 3) ? rk[3] : out[2])
				                 ^ SM4_CK[((rnd << 2) + (unsigned)k) & 0x1F];
				out[k] = sm4_key_l(rk[k], sm4_subword(b));
			}
			for (int k = 0; k < 4; k++)
				write_velem(regs, instr.rd, 32, i * 4 + k, out[k]);
			continue;
		}

		if (funct6 != 0x28 && funct6 != 0x29) return;
		// .vs takes its round keys from group zero for every block, .vv from
		// the matching group -- the same distinction the AES rounds draw.
		const uint64_t kg = (funct6 == 0x29) ? 0 : i;
		uint32_t rk[4], x[4], out[4];
		for (int k = 0; k < 4; k++) rk[k] = eg32(regs, instr.rs2, kg, 4, k);
		for (int k = 0; k < 4; k++) x[k] = eg32(regs, instr.rd, i, 4, k);
		for (int k = 0; k < 4; k++) {
			const uint32_t b = ((k < 1) ? x[1] : out[0])
			                 ^ ((k < 2) ? x[2] : out[1])
			                 ^ ((k < 3) ? x[3] : out[2])
			                 ^ rk[k];
			out[k] = sm4_round_l(x[k], sm4_subword(b));
		}
		for (int k = 0; k < 4; k++)
			write_velem(regs, instr.rd, 32, i * 4 + k, out[k]);
	}

	regs.set_vstart(0);
}

} // namespace vcommon
