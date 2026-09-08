// Zbc, Zbkb and Zbkx: the bit-manipulation instructions the scalar crypto
// extensions need but the ratified B extension left out.
//
// They live together because they share a shape -- pure register-to-register
// bit shuffling, no state, no traps -- and because they are always enabled
// together in practice: Zkn pulls in Zbkb and Zbkx, Zks pulls in Zbkb and
// Zbc. Each still gets its own flag, so -march can name one without
// implying the others.
//
//   Zbc    carry-less multiply. Multiplication in GF(2), where the partial
//          products are XORed instead of added, so there is no carry. It is
//          how AES-GCM computes its authentication tag and how CRCs are
//          computed a word at a time rather than a bit at a time.
//   Zbkb   the bit manipulation a cipher wants: packing halves into a word,
//          reversing bits inside each byte. Most of Zbkb is already in Zbb
//          (rev8, rotates, andn/orn/xnor) -- only these few are new.
//   Zbkx   crossbar permutations, which look up 4- or 8-bit fields of rs1
//          using rs2 as the indices. That is an S-box applied to a whole
//          register at once, in one instruction and in constant time.
//
// Constant time is the point of all of it. A cipher that branches or
// table-walks on key material leaks the key through timing; these do the
// same work for every input.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>

namespace {

// Carry-less product of two 64-bit values, low 64 bits. Every set bit of b
// contributes a shifted copy of a, combined with XOR because GF(2) addition
// is XOR -- which is exactly what makes it carry-less.
uint64_t clmul_lo(uint64_t a, uint64_t b)
{
	uint64_t r = 0;
	for (int i = 0; i < 64; i++)
		if ((b >> i) & 1) r ^= a << i;
	return r;
}

// The high half of the same 128-bit product. Shifting right by (64 - i)
// picks up the bits that fell off the top, and i == 0 is excluded because
// a >> 64 is undefined in C++ rather than zero.
uint64_t clmul_hi(uint64_t a, uint64_t b)
{
	uint64_t r = 0;
	for (int i = 1; i < 64; i++)
		if ((b >> i) & 1) r ^= a >> (64 - i);
	return r;
}

// Reverse the bits within each byte, leaving the bytes themselves in place.
// rev8 reverses the bytes and not the bits; brev8 is its complement, and a
// cipher that wants both applies them in sequence.
uint64_t brev8(uint64_t x)
{
	uint64_t r = 0;
	for (int byte = 0; byte < 8; byte++) {
		uint8_t v = (uint8_t)(x >> (byte * 8));
		uint8_t o = 0;
		for (int b = 0; b < 8; b++)
			if ((v >> b) & 1) o |= (uint8_t)(1u << (7 - b));
		r |= (uint64_t)o << (byte * 8);
	}
	return r;
}

// The crossbar. rs2 is read as a vector of n-bit indices; each index selects
// the n-bit field of rs1 to place at that position. An index past the end of
// rs1 yields zero, which is what makes a partial table safe to use.
uint64_t xperm(uint64_t rs1, uint64_t rs2, int width)
{
	const uint64_t mask = (width == 64) ? ~0ull : ((1ull << width) - 1);
	const int slots = 64 / width;
	uint64_t r = 0;
	for (int i = 0; i < slots; i++) {
		const uint64_t idx = (rs2 >> (i * width)) & mask;
		if (idx < (uint64_t)slots)
			r |= ((rs1 >> (idx * width)) & mask) << (i * width);
	}
	return r;
}

} // namespace

DecodedInstruction Decoder::decode_zbkb(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.length = 4;
	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	const bool op32 = instr.opcode == 0b0111011;
	switch (instr.funct7) {
	case 0b0000101:
		instr.ext = Extension::ZBC;
		instr.mnemonic = instr.funct3 == 0b001 ? "CLMUL"
		               : instr.funct3 == 0b010 ? "CLMULR" : "CLMULH";
		break;
	case 0b0010100:
		instr.ext = Extension::ZBKX;
		instr.mnemonic = instr.funct3 == 0b010 ? "XPERM4" : "XPERM8";
		break;
	case 0b0000100:
		instr.ext = Extension::ZBKB;
		instr.mnemonic = op32 ? "PACKW" : (instr.funct3 == 0b111 ? "PACKH" : "PACK");
		break;
	case 0b0110100:
		instr.ext = Extension::ZBKB;
		instr.mnemonic = "BREV8";
		break;
	default:
		instr.ext = Extension::ILLEGAL;
		break;
	}
	return instr;
}

void RiscvCore::exec_ZBKB(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	const uint64_t a = regs.read_x(instr.rs1);
	const uint64_t b = regs.read_x(instr.rs2);
	const bool op32 = instr.opcode == 0b0111011;
	uint64_t result = 0;

	switch (instr.funct7) {
	case 0b0000101:
		// clmulr is the *reversed* carry-less product: bit i of the result
		// is what clmul would put at bit 63-i of a bit-reversed input. It
		// exists because CRC polynomials are conventionally written in the
		// reversed bit order, and doing it here saves two brev8 pairs
		// around every step.
		if (instr.funct3 == 0b001)      result = clmul_lo(a, b);
		else if (instr.funct3 == 0b011) result = clmul_hi(a, b);
		else                            result = (clmul_hi(a, b) << 1) | (clmul_lo(a, b) >> 63);
		break;
	case 0b0010100:
		result = xperm(a, b, instr.funct3 == 0b010 ? 4 : 8);
		break;
	case 0b0000100:
		// pack takes the low half of each source and concatenates them,
		// rs1 low and rs2 high. packh does the same with single bytes,
		// zero-extended. packw is the 32-bit form and sign-extends, which
		// is why zext.h -- packw with rs2 = x0 -- belongs to Zbb and is
		// decoded there rather than here.
		if (op32)
			result = (uint64_t)(int64_t)(int32_t)((uint32_t)(a & 0xFFFF)
			                                    | ((uint32_t)(b & 0xFFFF) << 16));
		else if (instr.funct3 == 0b111)
			result = (a & 0xFF) | ((b & 0xFF) << 8);
		else
			result = (a & 0xFFFFFFFFull) | ((b & 0xFFFFFFFFull) << 32);
		break;
	case 0b0110100:
		result = brev8(a);
		break;
	default:
		break;
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(regs.get_pc() + instr.length);
}
