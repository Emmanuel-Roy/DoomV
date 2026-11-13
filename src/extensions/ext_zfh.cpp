// Zfh: full half-precision arithmetic.
//
// Zfhmin, next door, deliberately stops at load/store/move/convert -- that
// is all RVA23U64 mandates, and the design intent is that a guest wanting
// half arithmetic widens to single, computes, and narrows back. Zfh is the
// expansion option that lets it compute in half directly, and it is what
// riscv-tests exercises in its rv64uzfh family.
//
// Everything numeric goes through Berkeley SoftFloat's f16 routines, for the
// same reason F and D do -- see ext_softfloat.hpp, which lays out why
// computing on the host FPU is not trustworthy here. It matters more for
// half than for either: the format has an 11-bit significand, so a
// double-rounding error from computing in single and narrowing is not a
// corner case but a routine one, and writing a second rounding
// implementation by hand is precisely the mistake that wrapper exists to
// avoid.
//
// Encoding, from OP-FP's funct7: bits 26:25 are the format field, and half
// is 0b10 (single 00, double 01, quad 11). So every half-precision op is its
// single-precision counterpart's funct7 with those two bits set to 10, and
// the same holds for the fused-multiply opcodes' funct2. That regularity is
// why classify() can route the whole extension on one test.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "fpu.hpp"
#include "ext_xstate.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

DecodedInstruction Decoder::decode_zfh(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZFH;
	instr.length = 4;
	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;
	instr.rs3    = (raw_instr >> 27) & 0x1F;

	switch (instr.opcode) {
	case 0b1000011: instr.mnemonic = "FMADD.H";  return instr;
	case 0b1000111: instr.mnemonic = "FMSUB.H";  return instr;
	case 0b1001011: instr.mnemonic = "FNMSUB.H"; return instr;
	case 0b1001111: instr.mnemonic = "FNMADD.H"; return instr;
	default: break;
	}

	switch (instr.funct7) {
	case 0x02: instr.mnemonic = "FADD.H"; break;
	case 0x06: instr.mnemonic = "FSUB.H"; break;
	case 0x0A: instr.mnemonic = "FMUL.H"; break;
	case 0x0E: instr.mnemonic = "FDIV.H"; break;
	case 0x2E: instr.mnemonic = "FSQRT.H"; break;
	case 0x12: instr.mnemonic = instr.funct3 == 0 ? "FSGNJ.H"
	                          : instr.funct3 == 1 ? "FSGNJN.H" : "FSGNJX.H"; break;
	case 0x16: instr.mnemonic = instr.funct3 == 0 ? "FMIN.H" : "FMAX.H"; break;
	case 0x52: instr.mnemonic = instr.funct3 == 2 ? "FEQ.H"
	                          : instr.funct3 == 1 ? "FLT.H" : "FLE.H"; break;
	case 0x72: instr.mnemonic = "FCLASS.H"; break;
	case 0x62: instr.mnemonic = "FCVT.W.H"; break;
	case 0x6A: instr.mnemonic = "FCVT.H.W"; break;
	default:   instr.ext = Extension::ILLEGAL; break;
	}
	return instr;
}

void RiscvCore::exec_ZFH(const DecodedOp &instr, Registers &regs, Memory &mem)
{
	vcommon::FpDirtyWhenWritten fp_dirty(regs);   // FS: Dirty only if this writes FP state
	(void)mem;
	// Half precision is the third format of the same unit as F and D; every
	// Zfh instruction is OP-FP or a fused multiply-add with format 10.
	fpu::execute(instr, regs);
	regs.set_pc(regs.get_pc() + instr.length);
}
