#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"

Decoder::Decoder(RiscvCore &core, Registers &regs, Memory &mem)
	: core(core), regs(regs), mem(mem)
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
	case 0b0001111: // FENCE / FENCE.I
		return Extension::I;
	case 0b0110011: // OP: shared opcode, funct7 splits I (ADD/SUB/...) from M (MUL/DIV/...)
		return (funct7 == 0b0000001) ? Extension::M : Extension::I;
	case 0b0101111: // AMO
		return Extension::A;
	case 0b1110011: // SYSTEM: ECALL/EBREAK/CSR*, all gated behind Zicsr (disabled)
		return Extension::ZICSR;
	default:
		return Extension::ILLEGAL;
	}
}

DecodedInstruction Decoder::decode(uint32_t raw_instr, Extension ext) const
{
	DecodedInstruction instr{};
	instr.ext = ext;
	instr.length = 4;
	instr.mnemonic = "???";

	uint8_t opcode = raw_instr & 0x7F;
	uint8_t rd     = (raw_instr >> 7) & 0x1F;
	uint8_t funct3 = (raw_instr >> 12) & 0x07;
	uint8_t rs1    = (raw_instr >> 15) & 0x1F;
	uint8_t rs2    = (raw_instr >> 20) & 0x1F;
	uint8_t funct7 = (raw_instr >> 25) & 0x7F;

	instr.opcode = opcode;
	instr.rd = rd;
	instr.rs1 = rs1;
	instr.rs2 = rs2;
	instr.funct3 = funct3;
	instr.funct7 = funct7;

	// I-type: imm[11:0] = instr[31:20], sign-extended by the arithmetic shift.
	int32_t imm_i = (int32_t)raw_instr >> 20;

	// S-type: imm[11:5] = instr[31:25], imm[4:0] = instr[11:7].
	int32_t imm_s = (((raw_instr >> 25) & 0x7F) << 5) | ((raw_instr >> 7) & 0x1F);
	if (raw_instr & 0x80000000) imm_s |= 0xFFFFF000;

	// B-type: imm[12]=instr[31], imm[11]=instr[7], imm[10:5]=instr[30:25], imm[4:1]=instr[11:8], imm[0]=0.
	int32_t imm_b = (((raw_instr >> 31) & 0x1) << 12)
	              | (((raw_instr >> 7) & 0x1) << 11)
	              | (((raw_instr >> 25) & 0x3F) << 5)
	              | (((raw_instr >> 8) & 0xF) << 1);
	if (raw_instr & 0x80000000) imm_b |= 0xFFFFE000;

	// U-type: imm[31:12] = instr[31:12], imm[11:0] = 0.
	int32_t imm_u = raw_instr & 0xFFFFF000;

	// J-type: imm[20]=instr[31], imm[19:12]=instr[19:12], imm[11]=instr[20], imm[10:1]=instr[30:21], imm[0]=0.
	int32_t imm_j = (((raw_instr >> 31) & 0x1) << 20)
	              | (((raw_instr >> 12) & 0xFF) << 12)
	              | (((raw_instr >> 20) & 0x1) << 11)
	              | (((raw_instr >> 21) & 0x3FF) << 1);
	if (raw_instr & 0x80000000) imm_j |= 0xFFF00000;

	switch (opcode) {
	case 0b0110111: instr.mnemonic = "LUI";   instr.imm = imm_u; break;
	case 0b0010111: instr.mnemonic = "AUIPC"; instr.imm = imm_u; break;
	case 0b1101111: instr.mnemonic = "JAL";   instr.imm = imm_j; break;
	case 0b1100111: instr.mnemonic = "JALR";  instr.imm = imm_i; break;

	case 0b1100011: // Branch
		instr.imm = imm_b;
		switch (funct3) {
		case 0b000: instr.mnemonic = "BEQ";  break;
		case 0b001: instr.mnemonic = "BNE";  break;
		case 0b100: instr.mnemonic = "BLT";  break;
		case 0b101: instr.mnemonic = "BGE";  break;
		case 0b110: instr.mnemonic = "BLTU"; break;
		case 0b111: instr.mnemonic = "BGEU"; break;
		}
		break;

	case 0b0000011: // Load
		instr.imm = imm_i;
		switch (funct3) {
		case 0b000: instr.mnemonic = "LB";  break;
		case 0b001: instr.mnemonic = "LH";  break;
		case 0b010: instr.mnemonic = "LW";  break;
		case 0b100: instr.mnemonic = "LBU"; break;
		case 0b101: instr.mnemonic = "LHU"; break;
		}
		break;

	case 0b0100011: // Store
		instr.imm = imm_s;
		switch (funct3) {
		case 0b000: instr.mnemonic = "SB"; break;
		case 0b001: instr.mnemonic = "SH"; break;
		case 0b010: instr.mnemonic = "SW"; break;
		}
		break;

	case 0b0010011: // OP-IMM
		instr.imm = imm_i;
		switch (funct3) {
		case 0b000: instr.mnemonic = "ADDI";  break;
		case 0b010: instr.mnemonic = "SLTI";  break;
		case 0b011: instr.mnemonic = "SLTIU"; break;
		case 0b100: instr.mnemonic = "XORI";  break;
		case 0b110: instr.mnemonic = "ORI";   break;
		case 0b111: instr.mnemonic = "ANDI";  break;
		case 0b001: instr.mnemonic = "SLLI"; instr.imm = rs2; break; // shamt lives in the rs2 field
		case 0b101:
			instr.mnemonic = (funct7 == 0b0100000) ? "SRAI" : "SRLI";
			instr.imm = rs2;
			break;
		}
		break;

	case 0b0110011: // OP (R-type): I-side ADD/SUB/... or M-side MUL/DIV/..., no immediate
		if (ext == Extension::M) {
			switch (funct3) {
			case 0b000: instr.mnemonic = "MUL";    break;
			case 0b001: instr.mnemonic = "MULH";   break;
			case 0b010: instr.mnemonic = "MULHSU"; break;
			case 0b011: instr.mnemonic = "MULHU";  break;
			case 0b100: instr.mnemonic = "DIV";    break;
			case 0b101: instr.mnemonic = "DIVU";   break;
			case 0b110: instr.mnemonic = "REM";    break;
			case 0b111: instr.mnemonic = "REMU";   break;
			}
		} else {
			switch (funct3) {
			case 0b000: instr.mnemonic = (funct7 == 0b0100000) ? "SUB" : "ADD"; break;
			case 0b001: instr.mnemonic = "SLL";  break;
			case 0b010: instr.mnemonic = "SLT";  break;
			case 0b011: instr.mnemonic = "SLTU"; break;
			case 0b100: instr.mnemonic = "XOR";  break;
			case 0b101: instr.mnemonic = (funct7 == 0b0100000) ? "SRA" : "SRL"; break;
			case 0b110: instr.mnemonic = "OR";   break;
			case 0b111: instr.mnemonic = "AND";  break;
			}
		}
		break;

	case 0b0001111:
		instr.mnemonic = (funct3 == 0b001) ? "FENCE.I" : "FENCE";
		break;

	case 0b1110011:
		if (raw_instr == 0x00000073) instr.mnemonic = "ECALL";
		else if (raw_instr == 0x00100073) instr.mnemonic = "EBREAK";
		else instr.mnemonic = "CSR";
		instr.imm = imm_i;
		break;

	case 0b0101111: { // AMO: funct7[6:2] selects the op, funct7[1:0] are aq/rl
		uint8_t amo_op = funct7 >> 2;
		switch (amo_op) {
		case 0b00010: instr.mnemonic = "LR.W";      break;
		case 0b00011: instr.mnemonic = "SC.W";      break;
		case 0b00001: instr.mnemonic = "AMOSWAP.W"; break;
		case 0b00000: instr.mnemonic = "AMOADD.W";  break;
		case 0b00100: instr.mnemonic = "AMOXOR.W";  break;
		case 0b01100: instr.mnemonic = "AMOAND.W";  break;
		case 0b01000: instr.mnemonic = "AMOOR.W";   break;
		case 0b10000: instr.mnemonic = "AMOMIN.W";  break;
		case 0b10100: instr.mnemonic = "AMOMAX.W";  break;
		case 0b11000: instr.mnemonic = "AMOMINU.W"; break;
		case 0b11100: instr.mnemonic = "AMOMAXU.W"; break;
		}
		break;
	}

	default:
		break;
	}

	return instr;
}

DispatchResult Decoder::decode_and_dispatch(uint32_t raw_instr)
{
	Extension ext = classify(raw_instr);
	// Decode unconditionally, even for a disabled extension -- cheap (a
	// handful of shifts), and means an illegal instruction still shows a
	// real mnemonic in the dashboard/crash log instead of "???".
	DecodedInstruction instr = decode(raw_instr, ext);

	bool enabled = (ext == Extension::I && Extensions::I)
	            || (ext == Extension::M && Extensions::M)
	            || (ext == Extension::A && Extensions::A)
	            || (ext == Extension::C && Extensions::C)
	            || (ext == Extension::ZICSR && Extensions::ZICSR)
	            || (ext == Extension::V && Extensions::V);

	if (!enabled) {
		return {true, instr.mnemonic};
	}

	switch (ext) {
	case Extension::I:
		core.exec_32I(instr, regs, mem);
		break;
	case Extension::M:
		core.exec_32M(instr, regs, mem);
		break;
	case Extension::A:
		core.exec_32A(instr, regs, mem);
		break;
	default:
		return {true, instr.mnemonic};
	}

	return {false, instr.mnemonic};
}
