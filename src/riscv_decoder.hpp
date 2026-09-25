#pragma once
#include <cstdint>
#include <vector>

// One byte, so that it packs with the register fields in DecodedOp below.
enum class Extension : uint8_t {
	I,
	M,
	A,
	C,
	ZICSR,
	ZIFENCEI,
	F,
	D,
	V,
	ZBA,
	ZBB,
	ZBS,
	// The crypto bitmanip trio -- see ext_zbkb.cpp. Separate flags because
	// -march can name one without the others, even though Zkn and Zks pull
	// them in together.
	ZBC,
	ZBKB,
	ZBKX,
	ZICOND,
	ZIHINTPAUSE,
	ZIHINTNTL,
	ZIMOP,
	ZCMOP,
	ZICBOM,
	ZICBOP,
	ZICBOZ,
	ZAWRS,
	ZFA,
	ZFHMIN,
	// Full half-precision arithmetic, the expansion option on top of
	// Zfhmin -- see ext_zfh.cpp.
	ZFH,
	SVINVAL,
	H,
	ILLEGAL,
};

// What executing an instruction needs to know about it: everything a decode
// produces except its name. This is what the decode cache holds and what every
// exec_* function takes. 24 bytes, so that a cache entry -- this and the pc it
// was decoded at -- is 32 and never straddles a cache line.
struct DecodedOp {
	Extension ext;
	uint8_t opcode;
	uint8_t rd, rs1, rs2;
	uint8_t rs3;    // R4-type fourth operand -- only the fused multiply-add family (FMADD/FMSUB/FNMSUB/FNMADD) uses this
	uint8_t funct3, funct7;
	int64_t imm;    // sign-extends to full XLEN (64 bits)
	uint32_t raw;   // the instruction word as fetched (16 bits' worth for a compressed one).
	                // Only needed as the tval of an illegal-instruction trap, which has to
	                // report the encoding that was refused; filled in centrally by
	                // decode_and_dispatch so no per-extension decode has to remember to.
	uint8_t length; // 2 or 4 bytes
	bool word_op : 1;   // true for the *W-suffixed RV64 forms (ADDIW, SLLW, MULW, ...) -- 32-bit op, sign-extend result to 64
	bool op_64 : 1;     // true for the .D-suffixed RV64A forms (LR.D/SC.D/AMO*.D) -- selects 64-bit vs 32-bit memory width
	bool fp_double : 1; // true for F-extension instructions operating on double (D) instead of single (F) precision
	// Prototype: the flat operation DoomSystem::run_fast executes this as, set
	// when the decode is cached; 0 (FOP_SLOW) means "take the ordinary step".
	uint8_t fast_op;
};
static_assert(sizeof(DecodedOp) == 24, "DecodedOp is sized to make a decode cache entry 32 bytes");

// A decode as the decoders produce it: the operation, and its name for the
// dashboard. Only a fresh decode (Decoder::describe) is ever displayed, so the
// name stays out of the decode cache, where it was a third of every entry.
struct DecodedInstruction : DecodedOp {
	const char *mnemonic = "???"; // e.g. "ADDI" -- static string, name only, no operands. Defaulted so a
	                               // default-constructed DecodedInstruction (e.g. HistoryEntry's initial fill) is never a null pointer.
};

class Registers;
class Memory;
class RiscvCore;

struct DispatchResult {
	bool illegal;
	// The decode, in the decoder's cache: valid until the next
	// decode_and_dispatch. A pointer rather than a copy, because this is
	// returned for every instruction.
	const DecodedOp *decoded;
};

// Prototype: the operations DoomSystem::run_fast runs itself.
enum FastOp : uint8_t {
	FOP_SLOW = 0,
	FOP_LUI, FOP_AUIPC, FOP_JAL, FOP_JALR,
	FOP_BEQ, FOP_BNE, FOP_BLT, FOP_BGE, FOP_BLTU, FOP_BGEU,
	FOP_LB, FOP_LH, FOP_LW, FOP_LD, FOP_LBU, FOP_LHU, FOP_LWU,
	FOP_SB, FOP_SH, FOP_SW, FOP_SD,
	FOP_ADDI, FOP_SLTI, FOP_SLTIU, FOP_XORI, FOP_ORI, FOP_ANDI, FOP_SLLI, FOP_SRLI, FOP_SRAI,
	FOP_ADDIW, FOP_SLLIW, FOP_SRLIW, FOP_SRAIW,
	FOP_ADD, FOP_SUB, FOP_SLL, FOP_SLT, FOP_SLTU, FOP_XOR, FOP_SRL, FOP_SRA, FOP_OR, FOP_AND,
	FOP_ADDW, FOP_SUBW, FOP_SLLW, FOP_SRLW, FOP_SRAW,
	FOP_FENCE,  // also PAUSE: both only advance pc
	FOP_MEXT,   // any M instruction: exec_32M, which neither traps nor touches memory
};
FastOp classify_fast(const DecodedOp &d);

class Decoder {
	friend class DoomSystem;
public:
	Decoder(RiscvCore &core, Registers &regs, Memory &mem);

	DispatchResult decode_and_dispatch(uint64_t pc, uint32_t raw_instr);
	// What decode_and_dispatch would decode `raw` (as the history records
	// it: 16 bits for a compressed instruction) to, executing nothing.
	DecodedInstruction describe(uint32_t raw) const;

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
	DecodedInstruction decode_f(uint32_t raw_instr) const;
	DecodedInstruction decode_d(uint32_t raw_instr) const;
	DecodedInstruction decode_v(uint32_t raw_instr) const;
	DecodedInstruction decode_zba(uint32_t raw_instr) const;
	DecodedInstruction decode_zbb(uint32_t raw_instr) const;
	DecodedInstruction decode_zbs(uint32_t raw_instr) const;
	DecodedInstruction decode_zbkb(uint32_t raw_instr) const;
	DecodedInstruction decode_zfh(uint32_t raw_instr) const;
	DecodedInstruction decode_zicond(uint32_t raw_instr) const;
	DecodedInstruction decode_zifencei(uint32_t raw_instr) const;
	DecodedInstruction decode_zihintpause(uint32_t raw_instr) const;
	DecodedInstruction decode_zimop(uint32_t raw_instr) const;
	DecodedInstruction decode_zicbom(uint32_t raw_instr) const;
	DecodedInstruction decode_zicbop(uint32_t raw_instr) const;
	DecodedInstruction decode_zicboz(uint32_t raw_instr) const;
	DecodedInstruction decode_zfa(uint32_t raw_instr) const;
	DecodedInstruction decode_zfhmin(uint32_t raw_instr) const;
	DecodedInstruction decode_h_ldst(uint32_t raw_instr) const;
	// Zawrs has no decode_* of its own: wrs.nto/wrs.sto share SYSTEM
	// funct3=000 with ECALL/EBREAK/xRET/WFI and are separated by their
	// immediate, inside ext_zicsr.cpp's decode and exec switches.
	// The two compressed hint spaces, called from decode_compressed()
	// the same way the Zcb helpers are.
	DecodedInstruction decode_zihintntl(uint16_t raw16) const;
	DecodedInstruction decode_zcmop(uint16_t raw16) const;
	// Zcb lives in its own file but has no exec of its own: every encoding
	// is an alias, decoded into the standard instruction that executes it.
	DecodedInstruction decode_zcb_mem(uint16_t raw16) const;
	DecodedInstruction decode_zcb_alu(uint16_t raw16) const;

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
	// keyed by (addr, raw): the raw tag means a stale entry from
	// self-modified code just misses and re-decodes instead of silently
	// executing wrong bytes -- no separate invalidation needed. The tag is
	// the decode's own `raw`, which holds just the tagged bytes (16 bits for
	// a compressed entry, 32 for a standard one).
	//
	// An entry is 32 bytes, the pc and the DecodedOp, and an odd pc marks an
	// empty one, since no instruction starts at an odd address. The extension
	// set the decodes were made under is kept once for the whole cache
	// (cache_epoch) rather than in every entry, and when the set changes -- a
	// write to misa that actually changes it -- the cache is emptied. That is
	// also what lets an entry say whether its instruction may execute without
	// a flag of its own: one whose extension is disabled is cached with ext
	// ILLEGAL, and stays right for exactly as long as the entry does.
	//
	// 2^19 entries, 16 MB, covering a megabyte of code. A Linux boot runs
	// more distinct code than the 2^17 entries this used to have could hold:
	// 300M steps of the BusyBox boot missed 2.06M times at 2^17 and 0.65M at
	// 2^19, almost all of the difference an entry evicted by another pc.
	// Consecutive instructions take consecutive slots, and only the slots a
	// guest actually runs are ever touched, so DOOM pays nothing for the size.
	struct CacheEntry {
		uint64_t addr = EMPTY;
		DecodedOp decoded{};
	};
	static constexpr uint64_t EMPTY = 1;
	static_assert(sizeof(CacheEntry) == 32, "one decode cache entry, half a cache line");
	static constexpr uint32_t CACHE_BITS = 19;
	static constexpr uint32_t CACHE_SIZE = 1u << CACHE_BITS;
	static constexpr uint32_t CACHE_MASK = CACHE_SIZE - 1;
	std::vector<CacheEntry> cache;
	uint32_t cache_epoch = ~0u;   // the ExtensionsEpoch the cache holds decodes for
	// The extension set changed: empty the cache.
	void sync_extensions();
};
