#pragma once
#include <cstdint>
#include <vector>

enum class Extension {
	I,
	M,
	A,
	C,
	ZICSR,
	V,
	ILLEGAL,
};

struct DecodedInstruction {
	Extension ext;
	uint8_t opcode;
	uint8_t rd, rs1, rs2;
	uint8_t funct3, funct7;
	int32_t imm;
	uint8_t length; // 2 or 4 bytes
	const char *mnemonic; // e.g. "ADDI" -- static string, name only, no operands
};

class Registers;
class Memory;
class RiscvCore;

struct DispatchResult {
	bool illegal;
	const char *mnemonic;
	uint8_t length; // 2 or 4 -- lets the caller mask a compressed instruction's raw bytes correctly for history/trace display
};

class Decoder {
public:
	Decoder(RiscvCore &core, Registers &regs, Memory &mem);

	DispatchResult decode_and_dispatch(uint32_t pc, uint32_t raw_instr);

private:
	RiscvCore &core;
	Registers &regs;
	Memory &mem;

	// classify()/decode() figure out *what* an instruction is (opcode
	// tables, field extraction, mnemonic for display) -- exec_32I/M/A in
	// RiscvCore are what actually *do* something with it, and stay stubs.
	Extension classify(uint32_t raw_instr) const;
	DecodedInstruction decode(uint32_t raw_instr, Extension ext) const;

	// Compressed (RVC) instructions are entirely an encoding-space trick --
	// every one of them is defined as an alias for some standard 32-bit
	// instruction. So instead of giving RiscvCore a parallel exec_16C, this
	// just expands a 16-bit word into the equivalent DecodedInstruction
	// (same opcode/funct3/funct7/rd/rs1/rs2/imm fields a real 32-bit
	// encoding of that operation would produce, length=2 instead of 4) and
	// lets it flow through the existing exec_32I dispatch unchanged.
	DecodedInstruction decode_compressed(uint16_t raw16) const;

	// Hot loops (Doom's render/tic loop, memcpy-ish helpers, ...) execute
	// the same handful of addresses millions of times, redoing identical
	// classify()+decode() bitfield work every time. Direct-mapped cache
	// keyed by (addr, raw_instr): the raw_instr tag means a stale entry
	// from self-modified code just misses and re-decodes instead of
	// silently executing wrong bytes -- no separate invalidation needed.
	// raw_instr holds just the tagged bytes (16 bits for a compressed
	// entry, 32 for a standard one).
	struct CacheEntry {
		bool valid = false;
		uint32_t addr = 0;
		uint32_t raw_instr = 0;
		DecodedInstruction decoded{};
		bool enabled = false;
	};
	static constexpr uint32_t CACHE_BITS = 17;
	static constexpr uint32_t CACHE_SIZE = 1u << CACHE_BITS;
	static constexpr uint32_t CACHE_MASK = CACHE_SIZE - 1;
	std::vector<CacheEntry> cache;
};
