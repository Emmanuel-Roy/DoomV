// Zfhmin: minimal half-precision floating point.
//
// RVA23U64 mandates Zfhmin, not the full Zfh -- full half-precision
// *arithmetic* is an expansion option, not a requirement. So this file
// implements exactly what Zfhmin defines and no more: load and store,
// bit-pattern moves to and from an integer register, and conversion to and
// from the other float formats. There is deliberately no fadd.h here; a
// guest wanting half arithmetic is expected to widen to single, compute,
// and narrow back, which is the whole design of the extension.
//
// Encodings, from the assembler:
//
//   LOAD-FP  funct3 001              flh    (a width base F/D leaves unused)
//   STORE-FP funct3 001              fsh
//   OP-FP    funct7 0x72             fmv.x.h   (0x70 is FMV.X.W, 0x71 FMV.X.D)
//   OP-FP    funct7 0x7A             fmv.h.x   (0x78 is FMV.W.X, 0x79 FMV.D.X)
//   OP-FP    funct7 0x20, rs2 == 2   fcvt.s.h  (rs2 == 1 is FCVT.S.D)
//   OP-FP    funct7 0x21, rs2 == 2   fcvt.d.h  (rs2 == 0 is FCVT.D.S)
//   OP-FP    funct7 0x22, rs2 == 0   fcvt.h.s
//   OP-FP    funct7 0x22, rs2 == 1   fcvt.h.d
//
// The two conversions *into* half share funct7 0x20/0x21 with FCVT.S.D and
// FCVT.D.S, and 0x20 is also where Zfa's fround lives (rs2 4/5). Three
// extensions in one funct7, separated only by rs2 -- classify() has to check
// all of them explicitly, which is why the OP-FP arm there is as long as it
// is.
//
// The loads and stores are here; the moves and conversions are the shared
// floating-point unit's (fpu.cpp), as for F and D.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "mmu.hpp"
#include "fpu.hpp"
#include "ext_xstate.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zfhmin(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZFHMIN;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	if (instr.opcode == 0b0000111) { // flh -- I-type immediate
		instr.imm = (int64_t)((int32_t)raw_instr >> 20);
		instr.mnemonic = "FLH";
		return instr;
	}
	if (instr.opcode == 0b0100111) { // fsh -- S-type immediate
		int32_t imm = (int32_t)(((raw_instr >> 25) << 5) | ((raw_instr >> 7) & 0x1F));
		if (imm & 0x800) imm |= ~0xFFF;
		instr.imm = (int64_t)imm;
		instr.mnemonic = "FSH";
		return instr;
	}

	switch (instr.funct7) {
	case 0x72: instr.mnemonic = "FMV.X.H"; break;
	case 0x7A: instr.mnemonic = "FMV.H.X"; break;
	case 0x20: instr.mnemonic = "FCVT.S.H"; break;
	case 0x21: instr.mnemonic = "FCVT.D.H"; break;
	case 0x22: instr.mnemonic = (instr.rs2 == 0) ? "FCVT.H.S" : "FCVT.H.D"; break;
	}
	return instr;
}

void RiscvCore::exec_ZFHMIN(const DecodedOp &instr, Registers &regs, Memory &mem)
{
	vcommon::FpDirtyWhenWritten fp_dirty(regs);   // FS: Dirty only if this writes FP state

	switch (instr.opcode) {
	case 0b0000111: { // flh
		// As FLW does it -- see exec_F. Boxed on the way in, so a later fmv.x.h
		// or fcvt sees a valid half rather than the canonical NaN the unbox
		// rule would otherwise give.
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t v;
		if (!load_virtual(regs, mem, addr, 2, v)) return;
		regs.write_f_bits(instr.rd, fpu::box(v, 16));
		break;
	}

	case 0b0100111: { // fsh
		// Raw bits, with no NaN-box check. A store transfers the low 16 bits
		// exactly as they sit in the register -- the spec is explicit that
		// FSH does not modify what it transfers and does not canonicalise
		// NaNs. The unbox rule belongs to instructions that *interpret* the
		// value as a number; applying it here turned every improperly-boxed
		// pattern into 0x7E00 on the way to memory.
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;   // as FLH
		if (!store_virtual(regs, mem, addr, 2, (uint16_t)regs.read_f_bits(instr.rs2))) return;
		break;
	}

	default:
		// fmv.x.h, fmv.h.x, and the conversions to and from half: the unit's.
		fpu::execute(instr, regs);
		break;
	}

	regs.set_pc(regs.get_pc() + instr.length);
}
