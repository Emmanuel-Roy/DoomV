// I extension: the base integer ISA -- LUI/AUIPC/JAL/JALR/Branch/Load/Store/
// OP-IMM/OP-IMM-32/OP(I-side)/OP-32(I-side)/FENCE.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"

DecodedInstruction Decoder::decode_i(uint32_t raw_instr, Extension ext) const
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
	// Assigning an already-sign-extended int32_t into instr.imm (int64_t)
	// below sign-extends it again automatically -- no separate 64-bit-aware
	// computation needed for any of these.
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

	// U-type: imm[31:12] = instr[31:12], imm[11:0] = 0. Bit 31 already lands
	// in int32_t's sign bit, so converting to instr.imm (int64_t) sign-
	// extends through bit 63 for free -- exactly RV64 LUI/AUIPC semantics.
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
		case 0b011: instr.mnemonic = "LD";  if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break; // RV64
		case 0b100: instr.mnemonic = "LBU"; break;
		case 0b101: instr.mnemonic = "LHU"; break;
		case 0b110: instr.mnemonic = "LWU"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break; // RV64 -- zero-extended 32-bit load
		}
		break;

	case 0b0100011: // Store
		instr.imm = imm_s;
		switch (funct3) {
		case 0b000: instr.mnemonic = "SB"; break;
		case 0b001: instr.mnemonic = "SH"; break;
		case 0b010: instr.mnemonic = "SW"; break;
		case 0b011: instr.mnemonic = "SD"; if (!Extensions.XLEN64) instr.ext = Extension::ILLEGAL; break; // RV64
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
		case 0b001: instr.mnemonic = "SLLI"; instr.imm = rs2 | ((funct7 & 1) << 5); break; // shamt is 6 bits on RV64
		case 0b101:
			instr.mnemonic = ((funct7 & 0x7E) == 0b0100000) ? "SRAI" : "SRLI";
			instr.imm = rs2 | ((funct7 & 1) << 5);
			break;
		}
		break;

	case 0b0011011: // OP-IMM-32 (RV64): ADDIW/SLLIW/SRLIW/SRAIW -- 32-bit op, result sign-extended to 64
		instr.imm = imm_i;
		instr.word_op = true;
		switch (funct3) {
		case 0b000: instr.mnemonic = "ADDIW"; break;
		case 0b001: instr.mnemonic = "SLLIW"; instr.imm = rs2; break; // shamt is only 5 bits here (operates on w32)
		case 0b101:
			instr.mnemonic = (funct7 == 0b0100000) ? "SRAIW" : "SRLIW";
			instr.imm = rs2;
			break;
		}
		break;

	case 0b0110011: // OP (R-type), I-side: ADD/SUB/SLL/SLT/SLTU/XOR/SRA/SRL/OR/AND
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
		break;

	case 0b0111011: // OP-32 (RV64), I-side: ADDW/SUBW/SLLW/SRLW/SRAW
		instr.word_op = true;
		switch (funct3) {
		case 0b000: instr.mnemonic = (funct7 == 0b0100000) ? "SUBW" : "ADDW"; break;
		case 0b001: instr.mnemonic = "SLLW"; break;
		case 0b101: instr.mnemonic = (funct7 == 0b0100000) ? "SRAW" : "SRLW"; break;
		}
		break;

	case 0b0001111:
		instr.mnemonic = (funct3 == 0b001) ? "FENCE.I" : "FENCE";
		break;

	default:
		break;
	}

	return instr;
}

void RiscvCore::exec_32I(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();
	uint64_t next_pc = pc + instr.length;

	uint64_t rs1_val = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);
	int64_t rs1_s = (int64_t)rs1_val;
	int64_t rs2_s = (int64_t)rs2_val;
	uint64_t imm_u = (uint64_t)instr.imm;

	switch (instr.opcode) {
	case 0b0110111: // LUI
		regs.write_x(instr.rd, imm_u);
		break;

	case 0b0010111: // AUIPC
		regs.write_x(instr.rd, pc + imm_u);
		break;

	case 0b1101111: // JAL
		regs.write_x(instr.rd, next_pc);
		next_pc = pc + imm_u;
		break;

	case 0b1100111: // JALR
		{
			uint64_t target = (rs1_val + imm_u) & ~1ull;
			regs.write_x(instr.rd, next_pc);
			next_pc = target;
		}
		break;

	case 0b1100011: { // Branch
		bool taken = false;
		switch (instr.funct3) {
		case 0b000: taken = (rs1_val == rs2_val); break; // BEQ
		case 0b001: taken = (rs1_val != rs2_val); break; // BNE
		case 0b100: taken = (rs1_s < rs2_s); break;      // BLT
		case 0b101: taken = (rs1_s >= rs2_s); break;     // BGE
		case 0b110: taken = (rs1_val < rs2_val); break;  // BLTU
		case 0b111: taken = (rs1_val >= rs2_val); break; // BGEU
		}
		if (taken) next_pc = pc + imm_u;
		break;
	}

	case 0b0000011: { // Load
		uint64_t addr = rs1_val + imm_u;
		uint64_t val = 0;
		switch (instr.funct3) {
		case 0b000: val = (uint64_t)(int64_t)(int8_t)mem.read8(addr);   break; // LB
		case 0b001: val = (uint64_t)(int64_t)(int16_t)mem.read16(addr); break; // LH
		case 0b010: val = (uint64_t)(int64_t)(int32_t)mem.read32(addr); break; // LW -- sign-extends on RV64
		case 0b011: val = mem.read64(addr);                             break; // LD
		case 0b100: val = mem.read8(addr);                              break; // LBU
		case 0b101: val = mem.read16(addr);                             break; // LHU
		case 0b110: val = mem.read32(addr);                             break; // LWU -- zero-extends
		}
		regs.write_x(instr.rd, val);
		break;
	}

	case 0b0100011: { // Store
		uint64_t addr = rs1_val + imm_u;
		switch (instr.funct3) {
		case 0b000: // SB
			mem.write8(addr, (uint8_t)rs2_val);
			break;
		case 0b001: // SH
			mem.write8(addr, (uint8_t)(rs2_val & 0xFF));
			mem.write8(addr + 1, (uint8_t)((rs2_val >> 8) & 0xFF));
			break;
		case 0b010: // SW
			mem.write32(addr, (uint32_t)rs2_val);
			break;
		case 0b011: // SD
			mem.write64(addr, rs2_val);
			break;
		}
		break;
	}

	case 0b0010011: { // OP-IMM
		if (!Extensions.XLEN64) {
			// RV32: every integer op is inherently 32-bit -- there's no
			// separate word-suffixed opcode the way RV64 has OP-IMM-32,
			// this same opcode just always means 32-bit. Compute low-32,
			// sign-extend into the 64-bit container that backs the
			// register file regardless of XLEN (see extensions.hpp).
			uint32_t a = (uint32_t)rs1_val;
			uint32_t result32 = 0;
			switch (instr.funct3) {
			case 0b000: result32 = a + (uint32_t)instr.imm; break; // ADDI
			case 0b010: result32 = ((int32_t)a < (int32_t)instr.imm) ? 1 : 0; break; // SLTI
			case 0b011: result32 = (a < (uint32_t)instr.imm) ? 1 : 0; break; // SLTIU
			case 0b100: result32 = a ^ (uint32_t)instr.imm; break; // XORI
			case 0b110: result32 = a | (uint32_t)instr.imm; break; // ORI
			case 0b111: result32 = a & (uint32_t)instr.imm; break; // ANDI
			case 0b001: result32 = a << (instr.imm & 0x1F); break; // SLLI
			case 0b101:
				result32 = (instr.funct7 == 0b0100000)
				         ? (uint32_t)((int32_t)a >> (instr.imm & 0x1F))  // SRAI
				         : (a >> (instr.imm & 0x1F));                     // SRLI
				break;
			}
			regs.write_x(instr.rd, sext32(result32));
			break;
		}
		uint64_t result = 0;
		switch (instr.funct3) {
		case 0b000: result = rs1_val + imm_u; break; // ADDI
		case 0b010: result = (rs1_s < instr.imm) ? 1 : 0; break;   // SLTI
		case 0b011: result = (rs1_val < imm_u) ? 1 : 0; break; // SLTIU
		case 0b100: result = rs1_val ^ imm_u; break; // XORI
		case 0b110: result = rs1_val | imm_u; break; // ORI
		case 0b111: result = rs1_val & imm_u; break; // ANDI
		case 0b001: result = rs1_val << (instr.imm & 0x3F); break; // SLLI (imm holds 6-bit shamt)
		case 0b101:
			result = (instr.funct7 & 0x7E) == 0b0100000
			       ? (uint64_t)(rs1_s >> (instr.imm & 0x3F))  // SRAI
			       : (rs1_val >> (instr.imm & 0x3F));          // SRLI
			break;
		}
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b0011011: { // OP-IMM-32 (RV64): ADDIW/SLLIW/SRLIW/SRAIW -- 32-bit op, sign-extend result
		uint32_t a = (uint32_t)rs1_val;
		uint32_t result32 = 0;
		switch (instr.funct3) {
		case 0b000: result32 = a + (uint32_t)instr.imm; break; // ADDIW
		case 0b001: result32 = a << (instr.imm & 0x1F); break; // SLLIW (imm holds 5-bit shamt)
		case 0b101:
			result32 = (instr.funct7 == 0b0100000)
			         ? (uint32_t)((int32_t)a >> (instr.imm & 0x1F))  // SRAIW
			         : (a >> (instr.imm & 0x1F));                     // SRLIW
			break;
		}
		regs.write_x(instr.rd, sext32(result32));
		break;
	}

	case 0b0110011: { // OP (R-type, I-side only -- M-side goes through exec_32M)
		if (!Extensions.XLEN64) {
			// RV32: same reasoning as OP-IMM above -- this opcode is
			// always 32-bit here, there's no separate OP-32 in RV32.
			uint32_t a = (uint32_t)rs1_val, b = (uint32_t)rs2_val;
			uint32_t result32 = 0;
			switch (instr.funct3) {
			case 0b000: result32 = (instr.funct7 == 0b0100000) ? (a - b) : (a + b); break; // SUB/ADD
			case 0b001: result32 = a << (b & 0x1F); break; // SLL
			case 0b010: result32 = ((int32_t)a < (int32_t)b) ? 1 : 0; break; // SLT
			case 0b011: result32 = (a < b) ? 1 : 0; break; // SLTU
			case 0b100: result32 = a ^ b; break; // XOR
			case 0b101:
				result32 = (instr.funct7 == 0b0100000)
				         ? (uint32_t)((int32_t)a >> (b & 0x1F))  // SRA
				         : (a >> (b & 0x1F));                      // SRL
				break;
			case 0b110: result32 = a | b; break; // OR
			case 0b111: result32 = a & b; break; // AND
			}
			regs.write_x(instr.rd, sext32(result32));
			break;
		}
		uint64_t result = 0;
		switch (instr.funct3) {
		case 0b000: result = (instr.funct7 == 0b0100000) ? (rs1_val - rs2_val) : (rs1_val + rs2_val); break; // SUB/ADD
		case 0b001: result = rs1_val << (rs2_val & 0x3F); break; // SLL
		case 0b010: result = (rs1_s < rs2_s) ? 1 : 0; break;     // SLT
		case 0b011: result = (rs1_val < rs2_val) ? 1 : 0; break; // SLTU
		case 0b100: result = rs1_val ^ rs2_val; break;           // XOR
		case 0b101:
			result = (instr.funct7 == 0b0100000)
			       ? (uint64_t)(rs1_s >> (rs2_val & 0x3F))  // SRA
			       : (rs1_val >> (rs2_val & 0x3F));          // SRL
			break;
		case 0b110: result = rs1_val | rs2_val; break; // OR
		case 0b111: result = rs1_val & rs2_val; break; // AND
		}
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b0111011: { // OP-32 (RV64, I-side only): ADDW/SUBW/SLLW/SRLW/SRAW -- 32-bit op, sign-extend result
		uint32_t a = (uint32_t)rs1_val, b = (uint32_t)rs2_val;
		uint32_t result32 = 0;
		switch (instr.funct3) {
		case 0b000: result32 = (instr.funct7 == 0b0100000) ? (a - b) : (a + b); break; // SUBW/ADDW
		case 0b001: result32 = a << (b & 0x1F); break; // SLLW
		case 0b101:
			result32 = (instr.funct7 == 0b0100000)
			         ? (uint32_t)((int32_t)a >> (b & 0x1F))  // SRAW
			         : (a >> (b & 0x1F));                      // SRLW
			break;
		}
		regs.write_x(instr.rd, sext32(result32));
		break;
	}

	case 0b0001111: // FENCE / FENCE.I -- NOP, no icache/reordering to manage here
		break;

	default:
		break; // decoder gates unrecognized opcodes to illegal before this is ever called
	}

	regs.set_pc(next_pc);
}
