#include "riscv_decoder.hpp"
#include "extensions/ext_v_common.hpp"
#include "extensions/ext_xstate.hpp"
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
		return Extension::I;
	case 0b0010011: { // OP-IMM: ADDI/SLTI/... are plain I, but the two shift
		// funct3s share their encoding space with Zbb/Zbs immediate forms,
		// distinguished by funct6 (bits 31:26 -- RV64 shamt is 6 bits, so
		// funct7's low bit belongs to the shift amount, not the opcode).
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		uint8_t funct6 = (raw_instr >> 26) & 0x3F;
		if (funct3 == 0b001) {
			switch (funct6) {
			case 0b000000: return Extension::I;   // SLLI
			case 0b011000: return Extension::ZBB; // clz/ctz/cpop/sext.b/sext.h
			case 0b001010: return Extension::ZBS; // bseti
			case 0b010010: return Extension::ZBS; // bclri
			case 0b011010: return Extension::ZBS; // binvi
			default: return Extension::ILLEGAL;
			}
		}
		if (funct3 == 0b101) {
			switch (funct6) {
			case 0b000000: return Extension::I;   // SRLI
			case 0b010000: return Extension::I;   // SRAI
			case 0b011000: return Extension::ZBB; // rori
			case 0b001010: return Extension::ZBB; // orc.b
			case 0b011010: return Extension::ZBB; // rev8
			case 0b010010: return Extension::ZBS; // bexti
			default: return Extension::ILLEGAL;
			}
		}
		return Extension::I; // ADDI/SLTI/SLTIU/XORI/ORI/ANDI
	}
	case 0b0001111: { // FENCE (I) / FENCE.I (Zifencei) -- split by funct3
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		return (funct3 == 0b001) ? Extension::ZIFENCEI : Extension::I;
	}
	case 0b0011011: { // OP-IMM-32 (RV64 only): ADDIW/SLLIW/SRLIW/SRAIW, plus
		// Zba's slli.uw and Zbb's clzw/ctzw/cpopw/roriw in the same shift space.
		if (!Extensions.XLEN64) return Extension::ILLEGAL;
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		uint8_t funct6 = (raw_instr >> 26) & 0x3F;
		if (funct3 == 0b000) return Extension::I; // ADDIW
		if (funct3 == 0b001) {
			if (funct6 == 0b000000) return Extension::I;   // SLLIW
			if (funct6 == 0b000010) return Extension::ZBA; // slli.uw (6-bit shamt)
			if (funct6 == 0b011000) return Extension::ZBB; // clzw/ctzw/cpopw
			return Extension::ILLEGAL;
		}
		if (funct3 == 0b101) {
			if (funct6 == 0b000000 || funct6 == 0b010000) return Extension::I; // SRLIW/SRAIW
			if (funct6 == 0b011000) return Extension::ZBB; // roriw
			return Extension::ILLEGAL;
		}
		return Extension::ILLEGAL;
	}
	case 0b0110011: { // OP: funct7 splits I (ADD/SUB/...), M (MUL/DIV/...) and the bitmanip families.
		// Anything unrecognized is ILLEGAL rather than falling through to I:
		// this path used to check funct7 only for SUB/SRA, so an unimplemented
		// Zb* op silently executed as its base-I funct3 twin (sh3add as OR),
		// which is far worse than halting. See ext_zb.cpp.
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		switch (funct7) {
		case 0b0000001: return Extension::M;
		case 0b0000000: return Extension::I;
		case 0b0100000: // SUB/SRA are I; andn/orn/xnor share the funct7
			if (funct3 == 0b000 || funct3 == 0b101) return Extension::I;
			if (funct3 == 0b100 || funct3 == 0b110 || funct3 == 0b111) return Extension::ZBB;
			return Extension::ILLEGAL;
		case 0b0010000: // sh1add/sh2add/sh3add
			return (funct3 == 0b010 || funct3 == 0b100 || funct3 == 0b110) ? Extension::ZBA : Extension::ILLEGAL;
		case 0b0000101: // min/minu/max/maxu
			return (funct3 >= 0b100) ? Extension::ZBB : Extension::ILLEGAL;
		case 0b0110000: // rol/ror
			return (funct3 == 0b001 || funct3 == 0b101) ? Extension::ZBB : Extension::ILLEGAL;
		case 0b0100100: // bclr/bext
			return (funct3 == 0b001 || funct3 == 0b101) ? Extension::ZBS : Extension::ILLEGAL;
		case 0b0110100: // binv
			return (funct3 == 0b001) ? Extension::ZBS : Extension::ILLEGAL;
		case 0b0000111: // Zicond czero.eqz/czero.nez
			return (funct3 == 0b101 || funct3 == 0b111) ? Extension::ZICOND : Extension::ILLEGAL;
		case 0b0010100: // bset
			return (funct3 == 0b001) ? Extension::ZBS : Extension::ILLEGAL;
		default: return Extension::ILLEGAL;
		}
	}
	case 0b0111011: { // OP-32 (RV64 only): same shape as OP above
		if (!Extensions.XLEN64) return Extension::ILLEGAL;
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		switch (funct7) {
		case 0b0000001: return Extension::M;
		case 0b0000000: return Extension::I;
		case 0b0100000: // SUBW/SRAW
			return (funct3 == 0b000 || funct3 == 0b101) ? Extension::I : Extension::ILLEGAL;
		case 0b0000100: // add.uw (Zba) / zext.h (Zbb) -- same funct7, split by funct3
			if (funct3 == 0b000) return Extension::ZBA;
			if (funct3 == 0b100) return Extension::ZBB;
			return Extension::ILLEGAL;
		case 0b0010000: // sh{1,2,3}add.uw
			return (funct3 == 0b010 || funct3 == 0b100 || funct3 == 0b110) ? Extension::ZBA : Extension::ILLEGAL;
		case 0b0110000: // rolw/rorw
			return (funct3 == 0b001 || funct3 == 0b101) ? Extension::ZBB : Extension::ILLEGAL;
		default: return Extension::ILLEGAL;
		}
	}
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
	case 0b0010011: // OP-IMM
	case 0b0011011: // OP-IMM-32 -- all four share their space with the bitmanip families
		if (ext == Extension::ZBA || ext == Extension::ZBB || ext == Extension::ZBS || ext == Extension::ZICOND) return decode_zb(raw_instr, ext);
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
		       || (instr.ext == Extension::V && Extensions.V)
		       || (instr.ext == Extension::ZBA && Extensions.ZBA)
		       || (instr.ext == Extension::ZBB && Extensions.ZBB)
		       || (instr.ext == Extension::ZBS && Extensions.ZBS)
		       || (instr.ext == Extension::ZICOND && Extensions.ZICOND);

		entry = {true, pc, tag, instr, enabled};
	}

	if (!enabled) {
		return {true, instr};
	}

	// mstatus.VS is runtime state, not a build-time toggle, so this check
	// has to sit *after* the decode cache -- `enabled` above is cached per
	// (address, encoding) and would otherwise freeze whatever the vector
	// unit's enable happened to be the first time this address ran.
	if (instr.ext == Extension::V && !vcommon::vector_unit_enabled(regs)) {
		return {true, instr};
	}
	if ((instr.ext == Extension::F || instr.ext == Extension::D) && !vcommon::fp_unit_enabled(regs)) {
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
	case Extension::ZBA:
	case Extension::ZBB:
	case Extension::ZBS:
	case Extension::ZICOND:
		core.exec_ZB(instr, regs, mem);
		break;
	case Extension::V:
		core.exec_V(instr, regs, mem);
		break;
	default:
		return {true, instr};
	}

	return {false, instr};
}
