#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"

Decoder::Decoder(RiscvCore &core, Registers &regs, Memory &mem)
	: core(core), regs(regs), mem(mem), cache(CACHE_SIZE)
{
}

Extension Decoder::classify(uint32_t raw_instr) const
{
	uint8_t opcode = raw_instr & 0x7F;
	uint8_t funct7 = (raw_instr >> 25) & 0x7F;

	switch (opcode) {
	case 0b0110111: // LUI
	case 0b0010111: // AUIPC
	case 0b1101111: // JAL
	case 0b1100111: // JALR
	case 0b1100011: // Branch
	case 0b0000011: // Load
	case 0b0100011: // Store
	case 0b0010011: // OP-IMM
		return Extension::I;
	case 0b0001111: { // FENCE (I) / FENCE.I (Zifencei) -- split by funct3
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		return (funct3 == 0b001) ? Extension::ZIFENCEI : Extension::I;
	}
	case 0b0011011: // OP-IMM-32 (RV64 only): ADDIW/SLLIW/SRLIW/SRAIW -- doesn't exist in RV32's encoding space
		return Extensions.XLEN64 ? Extension::I : Extension::ILLEGAL;
	case 0b0110011: // OP: shared opcode, funct7 splits I (ADD/SUB/...) from M (MUL/DIV/...)
		return (funct7 == 0b0000001) ? Extension::M : Extension::I;
	case 0b0111011: // OP-32 (RV64 only): same funct7 split -- I (ADDW/SUBW/...) vs M (MULW/DIVW/...)
		if (!Extensions.XLEN64) return Extension::ILLEGAL;
		return (funct7 == 0b0000001) ? Extension::M : Extension::I;
	case 0b0101111: // AMO
		return Extension::A;
	case 0b1110011: // SYSTEM: ECALL/EBREAK/CSR*, all gated behind Zicsr
		return Extension::ZICSR;
	case 0b0000111: { // LOAD-FP: FLW (F) / FLD (D) / vector loads (V) -- share this opcode with no real
		// collision: F/D only ever use funct3 (the spec's "width" field) 010/011, V's vector-load
		// encoding only ever uses 000/101/110/111 (EEW 8/16/32/64), so the two spaces don't overlap.
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		if (funct3 == 0b011) return Extension::D;
		if (funct3 == 0b010) return Extension::F;
		if (funct3 == 0b000 || funct3 == 0b101 || funct3 == 0b110 || funct3 == 0b111) return Extension::V;
		return Extension::ILLEGAL;
	}
	case 0b0100111: { // STORE-FP: FSW (F) / FSD (D) / vector stores (V) -- same split as LOAD-FP above
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		if (funct3 == 0b011) return Extension::D;
		if (funct3 == 0b010) return Extension::F;
		if (funct3 == 0b000 || funct3 == 0b101 || funct3 == 0b110 || funct3 == 0b111) return Extension::V;
		return Extension::ILLEGAL;
	}
	case 0b1000011: // FMADD
	case 0b1000111: // FMSUB
	case 0b1001011: // FNMSUB
	case 0b1001111: // FNMADD -- funct2 (bits 26:25) splits single/double, same as OP-FP's funct7 bit0
		return (((raw_instr >> 25) & 0x3) == 0b01) ? Extension::D : Extension::F;
	case 0b1010011: // OP-FP: almost every op's funct7 has single at an even value, double at +1 --
		// except FCVT.S.D/FCVT.D.S (0x20/0x21), which the spec lists under D since both widths are involved.
		if (funct7 == 0b0100000 || funct7 == 0b0100001) return Extension::D;
		return (funct7 & 0x1) ? Extension::D : Extension::F;
	case 0b1010111: // OP-V: vector arithmetic and vset{i}vl{i} -- its own opcode, no sharing/collision
		return Extension::V;
	default:
		return Extension::ILLEGAL;
	}
}

// Thin dispatcher: routes to the per-extension decode_* helper that actually
// does the field extraction and mnemonic lookup (each lives in its matching
// ext_*.cpp alongside its RiscvCore::exec_* counterpart). OP/OP-32 are the
// only opcodes two extensions share -- ext (already computed by classify())
// picks which side of the split applies.
DecodedInstruction Decoder::decode(uint32_t raw_instr, Extension ext) const
{
	uint8_t opcode = raw_instr & 0x7F;

	switch (opcode) {
	case 0b0101111: // AMO
		return decode_a(raw_instr);
	case 0b1110011: // SYSTEM
		return decode_zicsr(raw_instr);
	case 0b0000111: // LOAD-FP / vector load -- ext (already split by classify()) picks the side
	case 0b0100111: // STORE-FP / vector store
		return (ext == Extension::V) ? decode_v(raw_instr) : decode_fd(raw_instr, ext);
	case 0b1000011: // FMADD
	case 0b1000111: // FMSUB
	case 0b1001011: // FNMSUB
	case 0b1001111: // FNMADD
	case 0b1010011: // OP-FP
		return decode_fd(raw_instr, ext);
	case 0b1010111: // OP-V
		return decode_v(raw_instr);
	case 0b0110011: // OP
	case 0b0111011: // OP-32
		return (ext == Extension::M) ? decode_m(raw_instr) : decode_i(raw_instr, ext);
	default:
		return decode_i(raw_instr, ext);
	}
}

DispatchResult Decoder::decode_and_dispatch(uint64_t pc, uint32_t raw_word)
{
	// Bit[1:0] of the first halfword being != 0b11 is what marks an
	// instruction as compressed -- checked before touching the cache, since
	// compressed instructions only need (and are only tagged by) their
	// 16-bit half, while a standard instruction needs the full word.
	bool is_compressed = Extensions.C && ((raw_word & 0x3) != 0x3);
	uint32_t tag = is_compressed ? (raw_word & 0xFFFF) : raw_word;

	// Indexed by halfword, not word: compressed instructions can start on
	// either 2-byte-aligned half of a 4-byte slot, so >>2 would alias two
	// unrelated addresses into one cache line half the time.
	CacheEntry &entry = cache[(pc >> 1) & CACHE_MASK];

	DecodedInstruction instr;
	bool enabled;
	if (entry.valid && entry.addr == pc && entry.raw_instr == tag) {
		instr = entry.decoded;
		enabled = entry.enabled;
	} else {
		if (is_compressed) {
			instr = decode_compressed((uint16_t)tag);
		} else {
			Extension ext = classify(raw_word);
			// Decode unconditionally, even for a disabled extension --
			// cheap (a handful of shifts), and means an illegal
			// instruction still shows a real mnemonic in the
			// dashboard/crash log instead of "???".
			instr = decode(raw_word, ext);
		}

		enabled = (instr.ext == Extension::I && Extensions.I)
		       || (instr.ext == Extension::M && Extensions.M)
		       || (instr.ext == Extension::A && Extensions.A)
		       || (instr.ext == Extension::C && Extensions.C)
		       || (instr.ext == Extension::ZICSR && Extensions.ZICSR)
		       || (instr.ext == Extension::ZIFENCEI && Extensions.ZIFENCEI)
		       || (instr.ext == Extension::F && Extensions.F)
		       || (instr.ext == Extension::D && Extensions.D)
		       || (instr.ext == Extension::V && Extensions.V);

		entry = {true, pc, tag, instr, enabled};
	}

	if (!enabled) {
		return {true, instr};
	}

	switch (instr.ext) {
	case Extension::I:
	case Extension::C: // every RVC instruction is an alias for a standard I-type/R-type/B-type/J-type op
		core.exec_32I(instr, regs, mem);
		break;
	case Extension::M:
		core.exec_32M(instr, regs, mem);
		break;
	case Extension::A:
		core.exec_32A(instr, regs, mem);
		break;
	case Extension::ZICSR:
		core.exec_32ZICSR(instr, regs, mem);
		break;
	case Extension::ZIFENCEI: // FENCE.I -- same no-op path as plain FENCE, see exec_32I
		core.exec_32I(instr, regs, mem);
		break;
	case Extension::F:
	case Extension::D:
		core.exec_FD(instr, regs, mem);
		break;
	case Extension::V:
		core.exec_V(instr, regs, mem);
		break;
	default:
		return {true, instr};
	}

	return {false, instr};
}
