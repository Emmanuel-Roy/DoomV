// F: single-precision floating point -- its decode, and its loads and
// stores. Everything else runs in the shared floating-point unit (fpu.cpp),
// which is SoftFloat on bit patterns; see fpu.hpp.
//
// F and D share every one of their opcodes. OP-FP splits on funct7's low
// bit (even is single, odd is double), LOAD-FP/STORE-FP on funct3, and the
// fused multiply-add family on funct2 -- all decided in
// Decoder::classify(), so this file only ever sees the single-precision
// side and never has to branch on width.
//
// The two conversions between the widths, FCVT.S.D and FCVT.D.S, live in
// ext_d.cpp: they involve both formats, and the spec lists them under D.
//
// Single-precision values live NaN-boxed in the 64-bit f registers; the
// unit reads and writes them through fpu::read / fpu::write, which box.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "ext_xstate.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include "fpu.hpp"

DecodedInstruction Decoder::decode_f(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::F;
	instr.length = 4;
	instr.mnemonic = "???";
	instr.fp_double = false;

	uint8_t opcode = raw_instr & 0x7F;
	uint8_t funct3 = (raw_instr >> 12) & 0x07;
	uint8_t rs2    = (raw_instr >> 20) & 0x1F;
	uint8_t funct7 = (raw_instr >> 25) & 0x7F;

	instr.opcode = opcode;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = rs2;
	instr.funct3 = funct3;
	instr.funct7 = funct7;

	int32_t imm_i = (int32_t)raw_instr >> 20;
	int32_t imm_s = (((raw_instr >> 25) & 0x7F) << 5) | ((raw_instr >> 7) & 0x1F);
	if (raw_instr & 0x80000000) imm_s |= 0xFFFFF000;

	switch (opcode) {
	case 0b0000111: // LOAD-FP
		instr.imm = imm_i;
		instr.mnemonic = "FLW";
		break;
	case 0b0100111: // STORE-FP
		instr.imm = imm_s;
		instr.mnemonic = "FSW";
		break;

	// Fused multiply-add: rs3 is the fourth operand, in funct7's high bits.
	case 0b1000011: instr.rs3 = funct7 >> 2; instr.mnemonic = "FMADD.S";  break;
	case 0b1000111: instr.rs3 = funct7 >> 2; instr.mnemonic = "FMSUB.S";  break;
	case 0b1001011: instr.rs3 = funct7 >> 2; instr.mnemonic = "FNMSUB.S"; break;
	case 0b1001111: instr.rs3 = funct7 >> 2; instr.mnemonic = "FNMADD.S"; break;

	case 0b1010011: // OP-FP -- funct7 selects the operation, rs2 the conversion type
		switch (funct7) {
		case 0b0000000: instr.mnemonic = "FADD.S";  break;
		case 0b0000100: instr.mnemonic = "FSUB.S";  break;
		case 0b0001000: instr.mnemonic = "FMUL.S";  break;
		case 0b0001100: instr.mnemonic = "FDIV.S";  break;
		case 0b0101100: instr.mnemonic = "FSQRT.S"; break;
		case 0b0010000:
			switch (funct3) {
			case 0b000: instr.mnemonic = "FSGNJ.S";  break;
			case 0b001: instr.mnemonic = "FSGNJN.S"; break;
			case 0b010: instr.mnemonic = "FSGNJX.S"; break;
			}
			break;
		case 0b0010100: instr.mnemonic = (funct3 == 0b001) ? "FMAX.S" : "FMIN.S"; break;
		case 0b1010000: // FLE/FLT/FEQ.S -- rd is an integer register
			switch (funct3) {
			case 0b000: instr.mnemonic = "FLE.S"; break;
			case 0b001: instr.mnemonic = "FLT.S"; break;
			case 0b010: instr.mnemonic = "FEQ.S"; break;
			}
			break;
		case 0b1100000: // FCVT.W/WU/L/LU.S -- rd integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.W.S";  break;
			case 0b00001: instr.mnemonic = "FCVT.WU.S"; break;
			case 0b00010: instr.mnemonic = "FCVT.L.S";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.LU.S"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1101000: // FCVT.S.W/WU/L/LU -- rs1 integer
			switch (rs2) {
			case 0b00000: instr.mnemonic = "FCVT.S.W";  break;
			case 0b00001: instr.mnemonic = "FCVT.S.WU"; break;
			case 0b00010: instr.mnemonic = "FCVT.S.L";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			case 0b00011: instr.mnemonic = "FCVT.S.LU"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break;
			}
			break;
		case 0b1110000: instr.mnemonic = (funct3 == 0b001) ? "FCLASS.S" : "FMV.X.W"; break;
		case 0b1111000: instr.mnemonic = "FMV.W.X"; break;
		}
		break;

	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_F(const DecodedOp &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	// The decoder already established mstatus.FS is not Off. Whether this
	// instruction makes it Dirty depends on what it writes; see
	// FpDirtyWhenWritten.
	vcommon::FpDirtyWhenWritten fp_dirty(regs);

	if (instr.opcode == 0b0000111) { // FLW -- rs1 is an integer base register
		// Through load_virtual/store_virtual with the access's real width, as an
		// integer access goes. Translating only the base address checked one
		// byte: an access straddling into an unmapped or read-only page went
		// ahead, reading or writing whatever followed the first page
		// physically, and PMP saw one byte of it. It also left the data caches
		// unfilled, so the fast loop never found a page only FP code used.
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		uint64_t v;
		if (!load_virtual(regs, mem, addr, 4, v)) return;
		regs.write_f_bits(instr.rd, fpu::box(v, 32));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b0100111) { // FSW -- the raw low bits, no unboxing
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;   // as FLW
		if (!store_virtual(regs, mem, addr, 4, (uint32_t)regs.read_f_bits(instr.rs2))) return;
		regs.set_pc(pc + instr.length);
		return;
	}

	// Everything else -- arithmetic, the fused forms, conversions, moves,
	// compares -- is the shared unit, at single precision.
	fpu::execute(instr, regs);
	regs.set_pc(pc + instr.length);
}
