// Zfa: additional floating-point instructions.
//
// Five small families, all in OP-FP, all sharing funct7 values with base
// F/D and separated by a secondary field. That sharing is the whole reason
// this file needs care -- every one of these sits next to an instruction it
// must not be confused with:
//
//   funct7 0x78 / 0x79, rs2==1     fli.s / fli.d      (rs2==0 is FMV.W.X/FMV.D.X)
//   funct7 0x14 / 0x15, funct3 2/3 fminm / fmaxm      (funct3 0/1 is FMIN/FMAX)
//   funct7 0x20 / 0x21, rs2 4/5    fround / froundnx  (rs2 1/0 is FCVT.S.D/FCVT.D.S)
//   funct7 0x61,        rs2==8     fcvtmod.w.d        (rs2==0 is FCVT.W.D)
//   funct7 0x50 / 0x51, funct3 4/5 fleq / fltq        (funct3 0/1/2 is FLE/FLT/FEQ)
//
// Encodings and the fli constant table both came out of the assembler, not
// from memory. The table was checked in the forward direction: each of the
// 32 values in fpu.cpp's FLI tables was assembled as `fli.d fa0, <value>` and confirmed to
// produce consecutive indices 0..31. A table like this is exactly what a
// remembered opcode listing gets subtly wrong -- the entries are not
// uniformly spaced, and indices 2..7 are powers of two while 8..15 are not.
//
// Three semantic details distinguish these from their base-ISA neighbours,
// and all three are the entire point of the respective instruction:
//
//   * fminm/fmaxm return the canonical NaN if *either* operand is NaN.
//     Base FMIN/FMAX return the non-NaN operand instead. Implementing
//     fminm as fmin would be right in every case except the one it exists
//     for.
//
//   * fleq/fltq are the *quiet* comparisons: they raise invalid only for a
//     signalling NaN, where FLE/FLT raise it for any NaN. Same result
//     value, different exception behaviour -- so a test that only checks
//     the result cannot tell them apart, and this file's test checks fflags.
//
//   * fcvtmod.w.d wraps modulo 2^32 instead of saturating. Every other
//     float-to-int conversion in RISC-V clamps out-of-range inputs to the
//     destination's extreme; this one deliberately does not, and NaN and
//     infinity produce 0 rather than the saturated value.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "fpu.hpp"
#include "ext_xstate.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zfa(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZFA;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	switch (instr.funct7) {
	case 0x78: instr.mnemonic = "FLI.S";  instr.fp_double = false; break;
	case 0x79: instr.mnemonic = "FLI.D";  instr.fp_double = true;  break;
	case 0x14: instr.mnemonic = (instr.funct3 == 2) ? "FMINM.S" : "FMAXM.S"; instr.fp_double = false; break;
	case 0x15: instr.mnemonic = (instr.funct3 == 2) ? "FMINM.D" : "FMAXM.D"; instr.fp_double = true;  break;
	case 0x20: instr.mnemonic = (instr.rs2 == 4) ? "FROUND.S" : "FROUNDNX.S"; instr.fp_double = false; break;
	case 0x21: instr.mnemonic = (instr.rs2 == 4) ? "FROUND.D" : "FROUNDNX.D"; instr.fp_double = true;  break;
	case 0x61: instr.mnemonic = "FCVTMOD.W.D"; instr.fp_double = true; break;
	case 0x50: instr.mnemonic = (instr.funct3 == 4) ? "FLEQ.S" : "FLTQ.S"; instr.fp_double = false; break;
	case 0x51: instr.mnemonic = (instr.funct3 == 4) ? "FLEQ.D" : "FLTQ.D"; instr.fp_double = true;  break;
	}
	return instr;
}

void RiscvCore::exec_ZFA(const DecodedOp &instr, Registers &regs, Memory &mem)
{
	vcommon::FpDirtyWhenWritten fp_dirty(regs);   // FS: Dirty only if this writes FP state
	(void)mem;
	// Every Zfa instruction is an OP-FP encoding beside a base one: fli
	// beside fmv.w.x (rs2 1), fminm/fmaxm beside fmin/fmax (funct3 2, 3),
	// fround/froundnx beside the format conversions (rs2 4, 5), fleq/fltq
	// beside fle/flt (funct3 4, 5), fcvtmod.w.d beside fcvt.w.d (rs2 8). The
	// shared unit implements them where they sit.
	fpu::execute(instr, regs);
	regs.set_pc(regs.get_pc() + instr.length);
}
