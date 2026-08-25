#pragma once
#include <cstdint>
#include <vector>

enum class Extension {
	I,
	M,
	A,
	C,
	ZICSR,
	ZIFENCEI,
	F,
	D,
	V,
	ILLEGAL,
};

struct DecodedInstruction {
	Extension ext;
	uint8_t opcode;
	uint8_t rd, rs1, rs2;
	uint8_t rs3;    // R4-type fourth operand -- only the fused multiply-add family (FMADD/FMSUB/FNMSUB/FNMADD) uses this
	uint8_t funct3, funct7;
	int64_t imm;    // sign-extends to full XLEN (64 bits)
	uint8_t length; // 2 or 4 bytes
	bool word_op;   // true for the *W-suffixed RV64 forms (ADDIW, SLLW, MULW, ...) -- 32-bit op, sign-extend result to 64
	bool op_64;     // true for the .D-suffixed RV64A forms (LR.D/SC.D/AMO*.D) -- selects 64-bit vs 32-bit memory width
	bool fp_double; // true for F-extension instructions operating on double (D) instead of single (F) precision
	const char *mnemonic = "???"; // e.g. "ADDI" -- static string, name only, no operands. Defaulted so a
	                               // default-constructed DecodedInstruction (e.g. HistoryEntry's initial fill) is never a null pointer.
};

class Registers;
class Memory;
class RiscvCore;

struct DispatchResult {
	bool illegal;
	DecodedInstruction decoded; // full decode, not just the mnemonic -- the caller (DoomSystem::step, for
	                             // history/trace-log recording) needs the operand fields too, to render more than a bare mnemonic.
};

class Decoder {
public:
	Decoder(RiscvCore &core, Registers &regs, Memory &mem);

	DispatchResult decode_and_dispatch(uint64_t pc, uint32_t raw_instr);

private:
	RiscvCore &core;
	Registers &regs;
	Memory &mem;

	// classify()/decode() figure out *what* an instruction is (opcode
	// tables, field extraction, mnemonic for display) -- exec_32I/M/A in
	// RiscvCore are what actually *do* something with it, and stay stubs.
	Extension classify(uint32_t raw_instr) const;
	DecodedInstruction decode(uint32_t raw_instr, Extension ext) const;

	// decode() is a thin dispatcher over these -- one per extension, each
	// living alongside its matching RiscvCore::exec_* in ext_*.cpp, and each
	// self-sufficiently re-extracting whatever raw_instr fields it needs
	// (same independent-extraction convention classify()/decode_compressed()
	// already use) rather than sharing a prelude across files.
	DecodedInstruction decode_i(uint32_t raw_instr, Extension ext) const;
	DecodedInstruction decode_m(uint32_t raw_instr) const;
	DecodedInstruction decode_a(uint32_t raw_instr) const;
	DecodedInstruction decode_zicsr(uint32_t raw_instr) const;
	DecodedInstruction decode_fd(uint32_t raw_instr, Extension ext) const;
	DecodedInstruction decode_v(uint32_t raw_instr) const;

	// Compressed (RVC) instructions are entirely an encoding-space trick --
	// every one of them is defined as an alias for some standard 32-bit
	// instruction. So instead of giving RiscvCore a parallel exec_16C, this
	// just expands a 16-bit word into the equivalent DecodedInstruction
	// (same opcode/funct3/funct7/rd/rs1/rs2/imm fields a real 32-bit
	// encoding of that operation would produce, length=2 instead of 4) and
	// lets it flow through the existing exec_32I dispatch unchanged. Note
	// several encodings mean different things on RV64 than RV32 (e.g.
	// quadrant-1 funct3=001 is C.JAL on RV32 but C.ADDIW on RV64) -- this
	// project targets RV64 only, so only the RV64 meaning is implemented.
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
		uint64_t addr = 0;
		uint32_t raw_instr = 0;
		DecodedInstruction decoded{};
		bool enabled = false;
	};
	static constexpr uint32_t CACHE_BITS = 17;
	static constexpr uint32_t CACHE_SIZE = 1u << CACHE_BITS;
	static constexpr uint32_t CACHE_MASK = CACHE_SIZE - 1;
	std::vector<CacheEntry> cache;
};
