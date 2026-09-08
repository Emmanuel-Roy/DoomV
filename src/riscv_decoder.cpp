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
		// Zicbop's prefetch.{i,r,w} are ORI with rd=x0 and imm[4:0] naming
		// the variant -- an encoding base I reserves as a HINT. Only claim it
		// when the extension is on: with it off the encoding is still a
		// perfectly legal (result-discarding) ORI, not an illegal instruction.
		if (funct3 == 0b110 && Extensions.ZICBOP && ((raw_instr >> 7) & 0x1F) == 0) {
			uint32_t sel = (raw_instr >> 20) & 0x1F;
			if (sel == 0 || sel == 1 || sel == 3) return Extension::ZICBOP;
		}
		return Extension::I; // ADDI/SLTI/SLTIU/XORI/ORI/ANDI
	}
	case 0b0001111: { // MISC-MEM: FENCE (I), FENCE.I (Zifencei), cbo.* (Zicbom) -- split by funct3
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		if (funct3 == 0b001) return Extension::ZIFENCEI;
		if (funct3 == 0b010) { // cbo.*, selected by imm: 0/1/2 are Zicbom, 4 is Zicboz
			uint32_t imm = (raw_instr >> 20) & 0xFFF;
			if (imm <= 2) return Extension::ZICBOM;
			if (imm == 4) return Extension::ZICBOZ;
			return Extension::ILLEGAL;
		}
		// PAUSE is FENCE pred=W, succ=none with rd/rs1/fm zero, so it already
		// retired as a plain FENCE. Classifying it separately is what lets
		// the dashboard name it and -march gate it.
		if (funct3 == 0b000 && Extensions.ZIHINTPAUSE && raw_instr == 0x0100000Fu)
			return Extension::ZIHINTPAUSE;
		return Extension::I;
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
	case 0b1110011: { // SYSTEM: ECALL/EBREAK/CSR* (Zicsr), plus Zimop's MOP.R/MOP.RR.
		// funct3=100 is a slot Zicsr never uses. Bit 31 marks the MOP space,
		// and bit 25 picks the one-source (MOP.R) or two-source (MOP.RR) form.
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		if (funct3 == 0b100) {
			// Shared space: Zimop's MOP.R sets bit 31, the hypervisor's
			// hlv/hsv leave it clear. Without this split hlv decodes as
			// a may-be-operation and quietly writes zero to rd instead
			// of reading guest memory.
			if (raw_instr & 0x80000000u) return Extension::ZIMOP;
			uint8_t f7 = (raw_instr >> 25) & 0x7F;
			if (Extensions.H && f7 >= 0x30 && f7 <= 0x37) return Extension::H;
			return Extension::ILLEGAL;
		}
		// Zawrs' wrs.nto/wrs.sto are two more fixed immediates in the
		// ECALL/EBREAK/xRET/WFI space. Base I reserves these, so gating
		// Zawrs off correctly makes them illegal rather than reverting them
		// to some other meaning.
		if (funct3 == 0b000 && (raw_instr == 0x00D00073u || raw_instr == 0x01D00073u))
			return Extensions.ZAWRS ? Extension::ZAWRS : Extension::ILLEGAL;
		// Svinval sits next to SFENCE.VMA (funct7 0x09) at 0x0B and 0x0C.
		if (funct3 == 0b000 && ((raw_instr >> 7) & 0x1F) == 0) {
			uint8_t f7 = (raw_instr >> 25) & 0x7F;
			// hfence.vvma (0x11) and hfence.gvma (0x31). The latter
			// shares its funct7 with hsv.b -- funct3 is what separates
			// them, which is why this test is inside the funct3==0 arm.
			if (Extensions.H && (f7 == 0x11 || f7 == 0x31)) return Extension::H;
			// hinval.vvma (0x13) and hinval.gvma (0x33) are the Svinval
			// forms of those two fences and carry the same privilege
			// rules. They were decoded nowhere at all, so a guest could
			// issue one and have it quietly succeed -- the exact hole
			// closing SINVAL.VMA was meant to prevent, one funct7 over.
			if (Extensions.H && Extensions.SVINVAL && (f7 == 0x13 || f7 == 0x33))
				return Extension::H;
			uint8_t rs2 = (raw_instr >> 20) & 0x1F;
			if (f7 == 0x0B) return Extensions.SVINVAL ? Extension::SVINVAL : Extension::ILLEGAL;
			if (f7 == 0x0C && ((raw_instr >> 15) & 0x1F) == 0 && (rs2 == 0 || rs2 == 1))
				return Extensions.SVINVAL ? Extension::SVINVAL : Extension::ILLEGAL;
		}
		return Extension::ZICSR;
	}
	case 0b0000111: { // LOAD-FP: FLW (F) / FLD (D) / vector loads (V) -- share this opcode with no real
		// collision: F/D only ever use funct3 (the spec's "width" field) 010/011, V's vector-load
		// encoding only ever uses 000/101/110/111 (EEW 8/16/32/64), so the two spaces don't overlap.
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		if (funct3 == 0b011) return Extension::D;
		if (funct3 == 0b010) return Extension::F;
		// Width 001 is the half-precision slot, which base F/D leave unused.
		if (funct3 == 0b001) return Extensions.ZFHMIN ? Extension::ZFHMIN : Extension::ILLEGAL;
		if (funct3 == 0b000 || funct3 == 0b101 || funct3 == 0b110 || funct3 == 0b111) return Extension::V;
		return Extension::ILLEGAL;
	}
	case 0b0100111: { // STORE-FP: FSW (F) / FSD (D) / FSH (Zfhmin) / vector stores (V)
		uint8_t funct3 = (raw_instr >> 12) & 0x07;
		if (funct3 == 0b011) return Extension::D;
		if (funct3 == 0b010) return Extension::F;
		if (funct3 == 0b001) return Extensions.ZFHMIN ? Extension::ZFHMIN : Extension::ILLEGAL;
		if (funct3 == 0b000 || funct3 == 0b101 || funct3 == 0b110 || funct3 == 0b111) return Extension::V;
		return Extension::ILLEGAL;
	}
	case 0b1000011: // FMADD
	case 0b1000111: // FMSUB
	case 0b1001011: // FNMSUB
	case 0b1001111: // FNMADD -- funct2 (bits 26:25) splits single/double, same as OP-FP's funct7 bit0
		return (((raw_instr >> 25) & 0x3) == 0b01) ? Extension::D : Extension::F;
	case 0b1010011: { // OP-FP: almost every op's funct7 has single at an even value, double at +1 --
		// except FCVT.S.D/FCVT.D.S (0x20/0x21), which the spec lists under D since both widths are involved.
		//
		// Zfa shares five funct7 values with F/D and is separated by a
		// secondary field in each case, so it has to be checked first --
		// otherwise fli lands on FMV.W.X, fminm on FMIN, and so on.
		uint8_t f3 = (raw_instr >> 12) & 0x07;
		uint8_t rs2 = (raw_instr >> 20) & 0x1F;
		if (Extensions.ZFA) {
			switch (funct7) {
			case 0x78: case 0x79: if (rs2 == 1) return Extension::ZFA; break;         // fli
			case 0x14: case 0x15: if (f3 == 2 || f3 == 3) return Extension::ZFA; break; // fminm/fmaxm
			case 0x20: case 0x21: if (rs2 == 4 || rs2 == 5) return Extension::ZFA; break; // fround/froundnx
			case 0x61: if (rs2 == 8) return Extension::ZFA; break;                    // fcvtmod.w.d
			case 0x50: case 0x51: if (f3 == 4 || f3 == 5) return Extension::ZFA; break; // fleq/fltq
			default: break;
			}
		}
		// Zfhmin shares 0x20/0x21 with FCVT.S.D/FCVT.D.S (rs2 == 2 is the
		// half form) and owns 0x22, 0x72 and 0x7A outright. Together with
		// Zfa above, funct7 0x20 alone carries three extensions separated
		// only by rs2.
		if (Extensions.ZFHMIN) {
			switch (funct7) {
			case 0x20: case 0x21: if (rs2 == 2) return Extension::ZFHMIN; break;
			case 0x22: if (rs2 == 0 || rs2 == 1) return Extension::ZFHMIN; break;
			case 0x72: case 0x7A: if (rs2 == 0) return Extension::ZFHMIN; break;
			default: break;
			}
		}
		if (funct7 == 0b0100000 || funct7 == 0b0100001) return Extension::D;
		return (funct7 & 0x1) ? Extension::D : Extension::F;
	}
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
		if (ext == Extension::H) return decode_h_ldst(raw_instr);
		return (ext == Extension::ZIMOP) ? decode_zimop(raw_instr) : decode_zicsr(raw_instr);
		// Zawrs falls through to decode_zicsr, which names it and keeps
		// ext as classify() set it -- see ext_zawrs.cpp.
	case 0b0001111: // MISC-MEM
		if (ext == Extension::ZICBOM) return decode_zicbom(raw_instr);
		if (ext == Extension::ZICBOZ) return decode_zicboz(raw_instr);
		if (ext == Extension::ZIHINTPAUSE) return decode_zihintpause(raw_instr);
		return decode_i(raw_instr, ext);
	case 0b0000111: // LOAD-FP / vector load -- ext (already split by classify()) picks the side
	case 0b0100111: // STORE-FP / vector store
		if (ext == Extension::ZFHMIN) return decode_zfhmin(raw_instr);
		return (ext == Extension::V) ? decode_v(raw_instr) : ((ext == Extension::D) ? decode_d(raw_instr) : decode_f(raw_instr));
	case 0b1000011: // FMADD
	case 0b1000111: // FMSUB
	case 0b1001011: // FNMSUB
	case 0b1001111: // FNMADD
	case 0b1010011: // OP-FP
		if (ext == Extension::ZFHMIN) return decode_zfhmin(raw_instr);
		if (ext == Extension::ZFA) return decode_zfa(raw_instr);
		return ((ext == Extension::D) ? decode_d(raw_instr) : decode_f(raw_instr));
	case 0b1010111: // OP-V
		return decode_v(raw_instr);
	case 0b0110011: // OP
	case 0b0111011: // OP-32
	case 0b0010011: // OP-IMM
	case 0b0011011: // OP-IMM-32 -- all four share their space with the bitmanip families
		if (ext == Extension::ZBA) return decode_zba(raw_instr);
		if (ext == Extension::ZBB) return decode_zbb(raw_instr);
		if (ext == Extension::ZBS) return decode_zbs(raw_instr);
		if (ext == Extension::ZICOND) return decode_zicond(raw_instr);
		if (ext == Extension::ZICBOP) return decode_zicbop(raw_instr);
		if (ext == Extension::ZIFENCEI) return decode_zifencei(raw_instr);
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
		instr.raw = tag;

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
		       || (instr.ext == Extension::ZICOND && Extensions.ZICOND)
		       || (instr.ext == Extension::ZIHINTPAUSE && Extensions.ZIHINTPAUSE)
		       || (instr.ext == Extension::ZIHINTNTL && Extensions.ZIHINTNTL)
		       || (instr.ext == Extension::ZIMOP && Extensions.ZIMOP)
		       || (instr.ext == Extension::ZCMOP && Extensions.ZCMOP)
		       || (instr.ext == Extension::ZICBOM && Extensions.ZICBOM)
		       || (instr.ext == Extension::ZICBOP && Extensions.ZICBOP)
		       || (instr.ext == Extension::ZICBOZ && Extensions.ZICBOZ)
		       || (instr.ext == Extension::ZAWRS && Extensions.ZAWRS)
		       || (instr.ext == Extension::ZFA && Extensions.ZFA)
		       || (instr.ext == Extension::ZFHMIN && Extensions.ZFHMIN)
		       || (instr.ext == Extension::SVINVAL && Extensions.SVINVAL)
		       || (instr.ext == Extension::H && Extensions.H);

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
	if ((instr.ext == Extension::F || instr.ext == Extension::D || instr.ext == Extension::ZFA
	     || instr.ext == Extension::ZFHMIN) && !vcommon::fp_unit_enabled(regs)) {
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
	case Extension::ZIFENCEI:
		core.exec_ZIFENCEI(instr, regs, mem);
		break;
	case Extension::F:
		core.exec_F(instr, regs, mem);
		break;
	case Extension::D:
		core.exec_D(instr, regs, mem);
		break;
	case Extension::ZBA:
		core.exec_ZBA(instr, regs, mem);
		break;
	case Extension::ZBB:
		core.exec_ZBB(instr, regs, mem);
		break;
	case Extension::ZBS:
		core.exec_ZBS(instr, regs, mem);
		break;
	case Extension::ZICOND:
		core.exec_ZICOND(instr, regs, mem);
		break;
	case Extension::ZIHINTPAUSE:
		core.exec_ZIHINTPAUSE(instr, regs, mem);
		break;
	case Extension::ZIHINTNTL:
		core.exec_ZIHINTNTL(instr, regs, mem);
		break;
	case Extension::ZIMOP:
		core.exec_ZIMOP(instr, regs, mem);
		break;
	case Extension::ZCMOP:
		core.exec_ZCMOP(instr, regs, mem);
		break;
	case Extension::ZICBOM:
		core.exec_ZICBOM(instr, regs, mem);
		break;
	case Extension::ZICBOP:
		core.exec_ZICBOP(instr, regs, mem);
		break;
	case Extension::ZICBOZ:
		core.exec_ZICBOZ(instr, regs, mem);
		break;
	case Extension::ZAWRS:
		core.exec_ZAWRS(instr, regs, mem);
		break;
	case Extension::ZFA:
		core.exec_ZFA(instr, regs, mem);
		break;
	case Extension::ZFHMIN:
		core.exec_ZFHMIN(instr, regs, mem);
		break;
	case Extension::SVINVAL:
		core.exec_SVINVAL(instr, regs, mem);
		break;
	case Extension::H:
		core.exec_H(instr, regs, mem);
		break;
	case Extension::V:
		core.exec_V(instr, regs, mem);
		break;
	default:
		return {true, instr};
	}

	return {false, instr};
}
