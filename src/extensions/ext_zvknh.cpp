// Zvknha / Zvknhb: vector SHA-2, for both digest sizes.
//
// Three instructions, all in OP-VE (0x77) with funct3=010 alongside AES:
//
//   funct6 0x2d  vsha2ms.vv   message schedule, four words at a time
//   funct6 0x2e  vsha2ch.vv   two compression rounds, high message words
//   funct6 0x2f  vsha2cl.vv   two compression rounds, low message words
//
// SEW picks the digest family rather than a separate opcode doing it:
// SEW=32 is SHA-256 (Zvknha), SEW=64 is SHA-512 (Zvknhb). The round
// structure is identical between them and only the six rotate constants
// differ, which is why one implementation covers both -- and why the spec
// bothered to make them the same instruction.
//
// The element group is four elements wide (EGW = 128 or 256 bits), so the
// loop counts groups and vl/vstart are divided by four, exactly as in the
// AES file next door. Element 0 of a group is its least significant end.
//
// A word on the register layout, because it is not the obvious one: the
// compression instructions do not hold {a..h} in order. vs2 carries
// {f,e,b,a} from element 0 up and vd carries {h,g,d,c}, and the result is
// written back as {f,e,b,a} -- the four words the *next* pair of rounds
// will need. The other four are unchanged by two rounds, so the encoding
// spends its register space on the ones that move. Transcribed from Sail's
// zvknhab_insts.sail rather than reasoned out.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "ext_v_common.hpp"
#include <cstdint>

namespace vcommon {
namespace {

inline uint64_t rotr(uint64_t x, unsigned n, int sew)
{
	if (sew == 32) {
		const uint32_t v = (uint32_t)x;
		n &= 31;
		return n ? (uint32_t)((v >> n) | (v << (32 - n))) : v;
	}
	n &= 63;
	return n ? ((x >> n) | (x << (64 - n))) : x;
}

inline uint64_t shr(uint64_t x, unsigned n, int sew)
{
	return (sew == 32) ? (uint64_t)((uint32_t)x >> n) : (x >> n);
}

inline uint64_t trunc(uint64_t x, int sew) { return (sew == 32) ? (uint32_t)x : x; }

// The four SHA-2 mixing functions. sigma feeds the message schedule, Sigma
// the compression rounds; each pair differs only in its rotate amounts,
// which is where the two digest sizes part company.
inline uint64_t sig0(uint64_t x, int sew)
{
	return (sew == 32) ? (rotr(x, 7, sew) ^ rotr(x, 18, sew) ^ shr(x, 3, sew))
	                   : (rotr(x, 1, sew) ^ rotr(x, 8, sew) ^ shr(x, 7, sew));
}

inline uint64_t sig1(uint64_t x, int sew)
{
	return (sew == 32) ? (rotr(x, 17, sew) ^ rotr(x, 19, sew) ^ shr(x, 10, sew))
	                   : (rotr(x, 19, sew) ^ rotr(x, 61, sew) ^ shr(x, 6, sew));
}

inline uint64_t sum0(uint64_t x, int sew)
{
	return (sew == 32) ? (rotr(x, 2, sew) ^ rotr(x, 13, sew) ^ rotr(x, 22, sew))
	                   : (rotr(x, 28, sew) ^ rotr(x, 34, sew) ^ rotr(x, 39, sew));
}

inline uint64_t sum1(uint64_t x, int sew)
{
	return (sew == 32) ? (rotr(x, 6, sew) ^ rotr(x, 11, sew) ^ rotr(x, 25, sew))
	                   : (rotr(x, 14, sew) ^ rotr(x, 18, sew) ^ rotr(x, 41, sew));
}

inline uint64_t ch(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (~x & z); }
inline uint64_t maj(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }

inline uint64_t eg(const Registers &regs, int base, int sew, uint64_t g, int k)
{
	return read_velem(regs, base, sew, g * 4 + (uint64_t)k);
}

} // namespace

void exec_zvknh(const DecodedInstruction &instr, Registers &regs)
{
	const VType vt = decode_vtype(regs.get_vtype());
	const int sew = vt.sew;
	if (sew != 32 && sew != 64) return;   // no other width is defined

	const uint8_t funct6 = op_v_funct6(instr.funct7);
	const uint64_t groups = regs.get_vl() / 4;
	const uint64_t start = regs.get_vstart() / 4;

	for (uint64_t i = start; i < groups; i++) {
		if (funct6 == 0x2d) { // vsha2ms.vv -- four new schedule words
			// Sail keeps a 20-slot window and fills only the twelve slots
			// the recurrence reads; the gaps are never touched. Naming them
			// w4 and w9..w15 rather than packing them tight keeps the four
			// lines below readable against the spec's own indices.
			uint64_t w[20] = {0};
			w[0] = eg(regs, instr.rd, sew, i, 0);
			w[1] = eg(regs, instr.rd, sew, i, 1);
			w[2] = eg(regs, instr.rd, sew, i, 2);
			w[3] = eg(regs, instr.rd, sew, i, 3);
			w[4] = eg(regs, instr.rs2, sew, i, 0);
			w[9] = eg(regs, instr.rs2, sew, i, 1);
			w[10] = eg(regs, instr.rs2, sew, i, 2);
			w[11] = eg(regs, instr.rs2, sew, i, 3);
			w[12] = eg(regs, instr.rs1, sew, i, 0);
			w[13] = eg(regs, instr.rs1, sew, i, 1);
			w[14] = eg(regs, instr.rs1, sew, i, 2);
			w[15] = eg(regs, instr.rs1, sew, i, 3);

			// Each of these four feeds the next: w[18] reads w[16]. That
			// dependency is why the instruction produces four words rather
			// than one, and why the additions must be truncated to SEW as
			// they go instead of at the end.
			w[16] = trunc(sig1(w[14], sew) + w[9] + sig0(w[1], sew) + w[0], sew);
			w[17] = trunc(sig1(w[15], sew) + w[10] + sig0(w[2], sew) + w[1], sew);
			w[18] = trunc(sig1(w[16], sew) + w[11] + sig0(w[3], sew) + w[2], sew);
			w[19] = trunc(sig1(w[17], sew) + w[12] + sig0(w[4], sew) + w[3], sew);

			for (int k = 0; k < 4; k++)
				write_velem(regs, instr.rd, sew, i * 4 + k, w[16 + k]);
			continue;
		}

		if (funct6 != 0x2e && funct6 != 0x2f) return;
		const bool low = (funct6 == 0x2f); // vsha2cl.vv

		uint64_t f = eg(regs, instr.rs2, sew, i, 0);
		uint64_t e = eg(regs, instr.rs2, sew, i, 1);
		uint64_t b = eg(regs, instr.rs2, sew, i, 2);
		uint64_t a = eg(regs, instr.rs2, sew, i, 3);
		uint64_t h = eg(regs, instr.rd, sew, i, 0);
		uint64_t g = eg(regs, instr.rd, sew, i, 1);
		uint64_t d = eg(regs, instr.rd, sew, i, 2);
		uint64_t c = eg(regs, instr.rd, sew, i, 3);

		// vs1 holds four message words already summed with their round
		// constants -- the K[t] addition happens in ordinary vector code, so
		// the crypto instruction never needs the constant table. The "high"
		// and "low" forms take the upper or lower pair of that quad, which
		// is what lets one 128-bit group cover four rounds across two
		// instructions.
		const uint64_t w0 = eg(regs, instr.rs1, sew, i, low ? 0 : 2);
		const uint64_t w1 = eg(regs, instr.rs1, sew, i, low ? 1 : 3);

		for (int round = 0; round < 2; round++) {
			const uint64_t wt = round ? w1 : w0;
			const uint64_t t1 = trunc(h + sum1(e, sew) + ch(e, f, g) + wt, sew);
			const uint64_t t2 = trunc(sum0(a, sew) + maj(a, b, c), sew);
			h = g; g = f; f = e;
			e = trunc(d + t1, sew);
			d = c; c = b; b = a;
			a = trunc(t1 + t2, sew);
		}

		write_velem(regs, instr.rd, sew, i * 4 + 0, f);
		write_velem(regs, instr.rd, sew, i * 4 + 1, e);
		write_velem(regs, instr.rd, sew, i * 4 + 2, b);
		write_velem(regs, instr.rd, sew, i * 4 + 3, a);
	}

	regs.set_vstart(0);
}

} // namespace vcommon
