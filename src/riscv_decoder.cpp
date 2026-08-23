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
	case 0b0011011: // OP-IMM-32 (RV64): ADDIW/SLLIW/SRLIW/SRAIW
	case 0b0001111: // FENCE / FENCE.I
		return Extension::I;
	case 0b0110011: // OP: shared opcode, funct7 splits I (ADD/SUB/...) from M (MUL/DIV/...)
		return (funct7 == 0b0000001) ? Extension::M : Extension::I;
	case 0b0111011: // OP-32 (RV64): shared opcode, same funct7 split -- I (ADDW/SUBW/...) vs M (MULW/DIVW/...)
		return (funct7 == 0b0000001) ? Extension::M : Extension::I;
	case 0b0101111: // AMO
		return Extension::A;
	case 0b1110011: // SYSTEM: ECALL/EBREAK/CSR*, all gated behind Zicsr
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
		case 0b011: instr.mnemonic = "LD";  break; // RV64
		case 0b100: instr.mnemonic = "LBU"; break;
		case 0b101: instr.mnemonic = "LHU"; break;
		case 0b110: instr.mnemonic = "LWU"; break; // RV64 -- zero-extended 32-bit load
		}
		break;

	case 0b0100011: // Store
		instr.imm = imm_s;
		switch (funct3) {
		case 0b000: instr.mnemonic = "SB"; break;
		case 0b001: instr.mnemonic = "SH"; break;
		case 0b010: instr.mnemonic = "SW"; break;
		case 0b011: instr.mnemonic = "SD"; break; // RV64
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

	case 0b0111011: // OP-32 (RV64): I-side ADDW/SUBW/SLLW/SRLW/SRAW or M-side MULW/DIVW/DIVUW/REMW/REMUW
		instr.word_op = true;
		if (ext == Extension::M) {
			switch (funct3) {
			case 0b000: instr.mnemonic = "MULW";  break;
			case 0b100: instr.mnemonic = "DIVW";  break;
			case 0b101: instr.mnemonic = "DIVUW"; break;
			case 0b110: instr.mnemonic = "REMW";  break;
			case 0b111: instr.mnemonic = "REMUW"; break;
			}
		} else {
			switch (funct3) {
			case 0b000: instr.mnemonic = (funct7 == 0b0100000) ? "SUBW" : "ADDW"; break;
			case 0b001: instr.mnemonic = "SLLW"; break;
			case 0b101: instr.mnemonic = (funct7 == 0b0100000) ? "SRAW" : "SRLW"; break;
			}
		}
		break;

	case 0b0001111:
		instr.mnemonic = (funct3 == 0b001) ? "FENCE.I" : "FENCE";
		break;

	case 0b1110011: {
		// imm[11:0] here is a CSR address (0-4095), not a signed immediate --
		// reusing imm_i would sign-extend and corrupt any address with bit
		// 11 set (e.g. mhartid = 0xF14).
		uint32_t csr_or_funct12 = (raw_instr >> 20) & 0xFFF;
		instr.imm = (int64_t)csr_or_funct12;
		if (funct3 == 0b000) {
			if (raw_instr == 0x00000073) instr.mnemonic = "ECALL";
			else if (raw_instr == 0x00100073) instr.mnemonic = "EBREAK";
			else if (raw_instr == 0x30200073) instr.mnemonic = "MRET";
			// else: unimplemented privileged op (WFI, SFENCE.VMA, ...) --
			// mnemonic stays "???"; exec_32ZICSR treats it as a no-op since
			// nothing in this project runs an OS that would emit one.
		} else {
			switch (funct3) {
			case 0b001: instr.mnemonic = "CSRRW";  break;
			case 0b010: instr.mnemonic = "CSRRS";  break;
			case 0b011: instr.mnemonic = "CSRRC";  break;
			case 0b101: instr.mnemonic = "CSRRWI"; break;
			case 0b110: instr.mnemonic = "CSRRSI"; break;
			case 0b111: instr.mnemonic = "CSRRCI"; break;
			}
		}
		break;
	}

	case 0b0101111: { // AMO: funct7[6:2] selects the op, funct3 selects width (010=.W, 011=.D), funct7[1:0] are aq/rl
		instr.op_64 = (funct3 == 0b011);
		uint8_t amo_op = funct7 >> 2;
		switch (amo_op) {
		case 0b00010: instr.mnemonic = instr.op_64 ? "LR.D"      : "LR.W";      break;
		case 0b00011: instr.mnemonic = instr.op_64 ? "SC.D"      : "SC.W";      break;
		case 0b00001: instr.mnemonic = instr.op_64 ? "AMOSWAP.D" : "AMOSWAP.W"; break;
		case 0b00000: instr.mnemonic = instr.op_64 ? "AMOADD.D"  : "AMOADD.W";  break;
		case 0b00100: instr.mnemonic = instr.op_64 ? "AMOXOR.D"  : "AMOXOR.W";  break;
		case 0b01100: instr.mnemonic = instr.op_64 ? "AMOAND.D"  : "AMOAND.W";  break;
		case 0b01000: instr.mnemonic = instr.op_64 ? "AMOOR.D"   : "AMOOR.W";   break;
		case 0b10000: instr.mnemonic = instr.op_64 ? "AMOMIN.D"  : "AMOMIN.W";  break;
		case 0b10100: instr.mnemonic = instr.op_64 ? "AMOMAX.D"  : "AMOMAX.W";  break;
		case 0b11000: instr.mnemonic = instr.op_64 ? "AMOMINU.D" : "AMOMINU.W"; break;
		case 0b11100: instr.mnemonic = instr.op_64 ? "AMOMAXU.D" : "AMOMAXU.W"; break;
		}
		break;
	}

	default:
		break;
	}

	return instr;
}

// Register-field helpers: compressed encodings either give a full 5-bit
// register number (quadrants 1/2's rd/rs1/rs2) or a 3-bit field restricted
// to x8-x15 (marked with a trailing prime in the ISA manual and in the
// comments below).
static inline uint8_t creg_full(uint16_t raw16, int shift) { return (uint8_t)((raw16 >> shift) & 0x1F); }
static inline uint8_t creg_prime(uint16_t raw16, int shift) { return (uint8_t)(((raw16 >> shift) & 0x7) + 8); }

DecodedInstruction Decoder::decode_compressed(uint16_t raw16) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::C;
	instr.length = 2;
	instr.mnemonic = "???";

	// All-zero is reserved as a permanently-illegal encoding by the spec --
	// a useful canary for accidentally executing zeroed/uninitialized
	// memory as code instead of a real trap.
	if (raw16 == 0) {
		instr.ext = Extension::ILLEGAL;
		return instr;
	}

	uint8_t quadrant = raw16 & 0x3;
	uint8_t funct3 = (raw16 >> 13) & 0x7;

	switch (quadrant) {
	case 0b00:
		switch (funct3) {
		case 0b000: { // C.ADDI4SPN -> ADDI rd', x2, nzuimm
			uint32_t imm = (((raw16 >> 7) & 0xF) << 6) | (((raw16 >> 11) & 0x3) << 4)
			             | (((raw16 >> 5) & 0x1) << 3) | (((raw16 >> 6) & 0x1) << 2);
			instr.mnemonic = "C.ADDI4SPN";
			instr.opcode = 0b0010011; instr.funct3 = 0b000;
			instr.rd = creg_prime(raw16, 2); instr.rs1 = 2; instr.imm = (int64_t)imm;
			break;
		}
		case 0b010: { // C.LW -> LW rd', imm(rs1')
			uint32_t imm = (((raw16 >> 5) & 0x1) << 6) | (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 6) & 0x1) << 2);
			instr.mnemonic = "C.LW";
			instr.opcode = 0b0000011; instr.funct3 = 0b010;
			instr.rd = creg_prime(raw16, 2); instr.rs1 = creg_prime(raw16, 7); instr.imm = (int64_t)imm;
			break;
		}
		case 0b011: { // C.LD (RV64) -> LD rd', imm(rs1')
			uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 5) & 0x3) << 6);
			instr.mnemonic = "C.LD";
			instr.opcode = 0b0000011; instr.funct3 = 0b011;
			instr.rd = creg_prime(raw16, 2); instr.rs1 = creg_prime(raw16, 7); instr.imm = (int64_t)imm;
			break;
		}
		case 0b110: { // C.SW -> SW rs2', imm(rs1')
			uint32_t imm = (((raw16 >> 5) & 0x1) << 6) | (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 6) & 0x1) << 2);
			instr.mnemonic = "C.SW";
			instr.opcode = 0b0100011; instr.funct3 = 0b010;
			instr.rs1 = creg_prime(raw16, 7); instr.rs2 = creg_prime(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		case 0b111: { // C.SD (RV64) -> SD rs2', imm(rs1')
			uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 5) & 0x3) << 6);
			instr.mnemonic = "C.SD";
			instr.opcode = 0b0100011; instr.funct3 = 0b011;
			instr.rs1 = creg_prime(raw16, 7); instr.rs2 = creg_prime(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		default:
			instr.ext = Extension::ILLEGAL;
			break;
		}
		break;

	case 0b01:
		switch (funct3) {
		case 0b000: { // rd==0,imm==0 -> C.NOP; else C.ADDI rd,rd,imm
			int32_t imm = (int32_t)(((raw16 >> 2) & 0x1F) | (((raw16 >> 12) & 0x1) << 5));
			if (imm & 0x20) imm |= ~0x3F;
			uint8_t rd = creg_full(raw16, 7);
			instr.mnemonic = (rd == 0 && imm == 0) ? "C.NOP" : "C.ADDI";
			instr.opcode = 0b0010011; instr.funct3 = 0b000;
			instr.rd = rd; instr.rs1 = rd; instr.imm = imm;
			break;
		}
		case 0b001: { // C.ADDIW (RV64) -> ADDIW rd,rd,imm -- same imm layout as C.ADDI (this slot is C.JAL on RV32)
			int32_t imm = (int32_t)(((raw16 >> 2) & 0x1F) | (((raw16 >> 12) & 0x1) << 5));
			if (imm & 0x20) imm |= ~0x3F;
			uint8_t rd = creg_full(raw16, 7);
			instr.mnemonic = "C.ADDIW";
			instr.opcode = 0b0011011; instr.funct3 = 0b000; instr.word_op = true;
			instr.rd = rd; instr.rs1 = rd; instr.imm = imm;
			break;
		}
		case 0b010: { // C.LI rd,imm -> ADDI rd, x0, imm
			int32_t imm = (int32_t)(((raw16 >> 2) & 0x1F) | (((raw16 >> 12) & 0x1) << 5));
			if (imm & 0x20) imm |= ~0x3F;
			instr.mnemonic = "C.LI";
			instr.opcode = 0b0010011; instr.funct3 = 0b000;
			instr.rd = creg_full(raw16, 7); instr.rs1 = 0; instr.imm = imm;
			break;
		}
		case 0b011: {
			uint8_t rd = creg_full(raw16, 7);
			if (rd == 2) { // C.ADDI16SP -> ADDI x2, x2, imm
				int32_t imm = (int32_t)(((raw16 >> 6) & 0x1) << 4 | ((raw16 >> 2) & 0x1) << 5
				            | ((raw16 >> 5) & 0x1) << 6 | ((raw16 >> 3) & 0x3) << 7 | ((raw16 >> 12) & 0x1) << 9);
				if (imm & 0x200) imm |= ~0x3FF;
				instr.mnemonic = "C.ADDI16SP";
				instr.opcode = 0b0010011; instr.funct3 = 0b000;
				instr.rd = 2; instr.rs1 = 2; instr.imm = imm;
			} else { // C.LUI rd,imm -> LUI rd,imm (nonzero rd, imm already positioned/sign-extended like a U-type imm)
				int32_t imm = (((raw16 >> 2) & 0x1F) << 12) | (int32_t)(((raw16 >> 12) & 0x1) << 17);
				if (imm & 0x20000) imm |= ~0x3FFFF;
				instr.mnemonic = "C.LUI";
				instr.opcode = 0b0110111; instr.rd = rd; instr.imm = imm;
			}
			break;
		}
		case 0b100: {
			uint8_t sub_op = (raw16 >> 10) & 0x3;
			uint8_t rd = creg_prime(raw16, 7);
			if (sub_op == 0b00 || sub_op == 0b01) { // C.SRLI / C.SRAI rd',shamt (6-bit shamt on RV64)
				uint32_t shamt = (((raw16 >> 12) & 0x1) << 5) | ((raw16 >> 2) & 0x1F);
				bool is_srai = (sub_op == 0b01);
				instr.mnemonic = is_srai ? "C.SRAI" : "C.SRLI";
				instr.opcode = 0b0010011; instr.funct3 = 0b101; instr.funct7 = is_srai ? 0b0100000 : 0b0000000;
				instr.rd = rd; instr.rs1 = rd; instr.imm = (int64_t)shamt;
			} else if (sub_op == 0b10) { // C.ANDI rd',rd',imm
				int32_t imm = (int32_t)(((raw16 >> 2) & 0x1F) | (((raw16 >> 12) & 0x1) << 5));
				if (imm & 0x20) imm |= ~0x3F;
				instr.mnemonic = "C.ANDI";
				instr.opcode = 0b0010011; instr.funct3 = 0b111;
				instr.rd = rd; instr.rs1 = rd; instr.imm = imm;
			} else if ((raw16 >> 12) & 0x1) { // funct1==1 -> C.SUBW/C.ADDW (RV64) or reserved
				uint8_t rs2 = creg_prime(raw16, 2);
				uint8_t funct2 = (raw16 >> 5) & 0x3;
				if (funct2 == 0b00 || funct2 == 0b01) {
					bool is_subw = (funct2 == 0b00);
					instr.mnemonic = is_subw ? "C.SUBW" : "C.ADDW";
					instr.opcode = 0b0111011; instr.funct3 = 0b000; instr.word_op = true;
					instr.funct7 = is_subw ? 0b0100000 : 0b0000000;
					instr.rd = rd; instr.rs1 = rd; instr.rs2 = rs2;
				} else {
					instr.ext = Extension::ILLEGAL; // funct2 10/11 reserved
				}
			} else { // C.SUB/C.XOR/C.OR/C.AND rd',rd',rs2'
				uint8_t rs2 = creg_prime(raw16, 2);
				uint8_t funct2 = (raw16 >> 5) & 0x3;
				static const char *names[4] = {"C.SUB", "C.XOR", "C.OR", "C.AND"};
				static const uint8_t funct3s[4] = {0b000, 0b100, 0b110, 0b111};
				instr.mnemonic = names[funct2];
				instr.opcode = 0b0110011; instr.funct3 = funct3s[funct2];
				instr.funct7 = (funct2 == 0) ? 0b0100000 : 0b0000000;
				instr.rd = rd; instr.rs1 = rd; instr.rs2 = rs2;
			}
			break;
		}
		case 0b101: { // C.J -> JAL x0, imm
			int32_t imm = (int32_t)(((raw16 >> 3) & 0x7) << 1 | ((raw16 >> 11) & 0x1) << 4 | ((raw16 >> 2) & 0x1) << 5
			            | ((raw16 >> 7) & 0x1) << 6 | ((raw16 >> 6) & 0x1) << 7 | ((raw16 >> 9) & 0x3) << 8
			            | ((raw16 >> 8) & 0x1) << 10 | ((raw16 >> 12) & 0x1) << 11);
			if (imm & 0x800) imm |= ~0xFFF;
			instr.mnemonic = "C.J";
			instr.opcode = 0b1101111; instr.rd = 0; instr.imm = imm;
			break;
		}
		case 0b110: // C.BEQZ rs1',imm -> BEQ rs1', x0, imm
		case 0b111: { // C.BNEZ rs1',imm -> BNE rs1', x0, imm
			int32_t imm = (int32_t)(((raw16 >> 3) & 0x3) << 1 | ((raw16 >> 10) & 0x3) << 3
			            | ((raw16 >> 2) & 0x1) << 5 | ((raw16 >> 5) & 0x3) << 6 | ((raw16 >> 12) & 0x1) << 8);
			if (imm & 0x100) imm |= ~0x1FF;
			bool is_bnez = (funct3 == 0b111);
			instr.mnemonic = is_bnez ? "C.BNEZ" : "C.BEQZ";
			instr.opcode = 0b1100011; instr.funct3 = is_bnez ? 0b001 : 0b000;
			instr.rs1 = creg_prime(raw16, 7); instr.rs2 = 0; instr.imm = imm;
			break;
		}
		}
		break;

	case 0b10:
		switch (funct3) {
		case 0b000: { // C.SLLI rd,shamt -> SLLI rd,rd,shamt (6-bit shamt on RV64)
			uint32_t shamt = (((raw16 >> 12) & 0x1) << 5) | ((raw16 >> 2) & 0x1F);
			uint8_t rd = creg_full(raw16, 7);
			instr.mnemonic = "C.SLLI";
			instr.opcode = 0b0010011; instr.funct3 = 0b001;
			instr.rd = rd; instr.rs1 = rd; instr.imm = (int64_t)shamt;
			break;
		}
		case 0b010: { // C.LWSP rd,imm -> LW rd, imm(x2)
			uint32_t imm = (((raw16 >> 4) & 0x7) << 2) | (((raw16 >> 12) & 0x1) << 5) | (((raw16 >> 2) & 0x3) << 6);
			instr.mnemonic = "C.LWSP";
			instr.opcode = 0b0000011; instr.funct3 = 0b010;
			instr.rd = creg_full(raw16, 7); instr.rs1 = 2; instr.imm = (int64_t)imm;
			break;
		}
		case 0b011: { // C.LDSP (RV64) -> LD rd, imm(x2)
			uint32_t imm = (((raw16 >> 5) & 0x3) << 3) | (((raw16 >> 12) & 0x1) << 5) | (((raw16 >> 2) & 0x7) << 6);
			instr.mnemonic = "C.LDSP";
			instr.opcode = 0b0000011; instr.funct3 = 0b011;
			instr.rd = creg_full(raw16, 7); instr.rs1 = 2; instr.imm = (int64_t)imm;
			break;
		}
		case 0b100: {
			uint8_t rd_rs1 = creg_full(raw16, 7);
			uint8_t rs2 = creg_full(raw16, 2);
			bool funct1 = (raw16 >> 12) & 0x1;
			if (!funct1 && rs2 == 0) { // C.JR rs1 -> JALR x0, 0(rs1)
				instr.mnemonic = "C.JR";
				instr.opcode = 0b1100111; instr.funct3 = 0b000;
				instr.rd = 0; instr.rs1 = rd_rs1; instr.imm = 0;
			} else if (!funct1) { // C.MV rd,rs2 -> ADD rd, x0, rs2
				instr.mnemonic = "C.MV";
				instr.opcode = 0b0110011; instr.funct3 = 0b000;
				instr.rd = rd_rs1; instr.rs1 = 0; instr.rs2 = rs2;
			} else if (rd_rs1 == 0 && rs2 == 0) { // C.EBREAK -- same fields a real EBREAK would decode to, so exec_32ZICSR handles it identically
				instr.mnemonic = "C.EBREAK";
				instr.ext = Extension::ZICSR;
				instr.opcode = 0b1110011; instr.funct3 = 0b000; instr.imm = 0x001;
			} else if (rs2 == 0) { // C.JALR rs1 -> JALR x1, 0(rs1)
				instr.mnemonic = "C.JALR";
				instr.opcode = 0b1100111; instr.funct3 = 0b000;
				instr.rd = 1; instr.rs1 = rd_rs1; instr.imm = 0;
			} else { // C.ADD rd,rd,rs2 -> ADD rd,rd,rs2
				instr.mnemonic = "C.ADD";
				instr.opcode = 0b0110011; instr.funct3 = 0b000;
				instr.rd = rd_rs1; instr.rs1 = rd_rs1; instr.rs2 = rs2;
			}
			break;
		}
		case 0b110: { // C.SWSP rs2,imm -> SW rs2, imm(x2)
			uint32_t imm = (((raw16 >> 9) & 0xF) << 2) | (((raw16 >> 7) & 0x3) << 6);
			instr.mnemonic = "C.SWSP";
			instr.opcode = 0b0100011; instr.funct3 = 0b010;
			instr.rs1 = 2; instr.rs2 = creg_full(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		case 0b111: { // C.SDSP (RV64) -> SD rs2, imm(x2)
			uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 7) & 0x7) << 6);
			instr.mnemonic = "C.SDSP";
			instr.opcode = 0b0100011; instr.funct3 = 0b011;
			instr.rs1 = 2; instr.rs2 = creg_full(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		default:
			instr.ext = Extension::ILLEGAL;
			break;
		}
		break;
	}

	return instr;
}

DispatchResult Decoder::decode_and_dispatch(uint64_t pc, uint32_t raw_word)
{
	// Bit[1:0] of the first halfword being != 0b11 is what marks an
	// instruction as compressed -- checked before touching the cache, since
	// compressed instructions only need (and are only tagged by) their
	// 16-bit half, while a standard instruction needs the full word.
	bool is_compressed = Extensions::C && ((raw_word & 0x3) != 0x3);
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

		enabled = (instr.ext == Extension::I && Extensions::I)
		       || (instr.ext == Extension::M && Extensions::M)
		       || (instr.ext == Extension::A && Extensions::A)
		       || (instr.ext == Extension::C && Extensions::C)
		       || (instr.ext == Extension::ZICSR && Extensions::ZICSR)
		       || (instr.ext == Extension::V && Extensions::V);

		entry = {true, pc, tag, instr, enabled};
	}

	if (!enabled) {
		return {true, instr.mnemonic, instr.length};
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
	default:
		return {true, instr.mnemonic, instr.length};
	}

	return {false, instr.mnemonic, instr.length};
}
