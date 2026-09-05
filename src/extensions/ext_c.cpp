// C extension: compressed 16-bit instructions. Every one of them is an
// alias for some standard 32-bit instruction -- decode_compressed() expands
// a 16-bit word into the equivalent DecodedInstruction (same fields a real
// 32-bit encoding would produce, length=2) and lets it flow through the
// existing exec_32I/exec_32ZICSR/exec_FD dispatch unchanged, so there's no
// separate exec_16C in RiscvCore.
#include "riscv_decoder.hpp"
#include "extensions.hpp"

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
		case 0b001: { // C.FLD (needs D) -> FLD rd', imm(rs1') -- same imm layout as C.LD, always available regardless of XLEN
			if (!Extensions.D) { instr.ext = Extension::ILLEGAL; break; }
			uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 5) & 0x3) << 6);
			instr.mnemonic = "C.FLD";
			instr.ext = Extension::D;
			instr.opcode = 0b0000111; instr.funct3 = 0b011; instr.fp_double = true;
			instr.rd = creg_prime(raw16, 2); instr.rs1 = creg_prime(raw16, 7); instr.imm = (int64_t)imm;
			break;
		}
		case 0b011: { // RV64: C.LD -> LD rd', imm(rs1'). RV32: this slot is C.FLW instead (needs F)
			if (Extensions.XLEN64) {
				uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 5) & 0x3) << 6);
				instr.mnemonic = "C.LD";
				instr.opcode = 0b0000011; instr.funct3 = 0b011;
				instr.rd = creg_prime(raw16, 2); instr.rs1 = creg_prime(raw16, 7); instr.imm = (int64_t)imm;
			} else if (Extensions.F) { // same imm layout as C.LW
				uint32_t imm = (((raw16 >> 5) & 0x1) << 6) | (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 6) & 0x1) << 2);
				instr.mnemonic = "C.FLW";
				instr.ext = Extension::F;
				instr.opcode = 0b0000111; instr.funct3 = 0b010; instr.fp_double = false;
				instr.rd = creg_prime(raw16, 2); instr.rs1 = creg_prime(raw16, 7); instr.imm = (int64_t)imm;
			} else {
				instr.ext = Extension::ILLEGAL;
			}
			break;
		}
		case 0b101: { // C.FSD (needs D) -> FSD rs2', imm(rs1') -- same imm layout as C.SD
			if (!Extensions.D) { instr.ext = Extension::ILLEGAL; break; }
			uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 5) & 0x3) << 6);
			instr.mnemonic = "C.FSD";
			instr.ext = Extension::D;
			instr.opcode = 0b0100111; instr.funct3 = 0b011; instr.fp_double = true;
			instr.rs1 = creg_prime(raw16, 7); instr.rs2 = creg_prime(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		case 0b110: { // C.SW -> SW rs2', imm(rs1')
			uint32_t imm = (((raw16 >> 5) & 0x1) << 6) | (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 6) & 0x1) << 2);
			instr.mnemonic = "C.SW";
			instr.opcode = 0b0100011; instr.funct3 = 0b010;
			instr.rs1 = creg_prime(raw16, 7); instr.rs2 = creg_prime(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		case 0b111: { // RV64: C.SD -> SD rs2', imm(rs1'). RV32: this slot is C.FSW instead (needs F)
			if (Extensions.XLEN64) {
				uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 5) & 0x3) << 6);
				instr.mnemonic = "C.SD";
				instr.opcode = 0b0100011; instr.funct3 = 0b011;
				instr.rs1 = creg_prime(raw16, 7); instr.rs2 = creg_prime(raw16, 2); instr.imm = (int64_t)imm;
			} else if (Extensions.F) { // same imm layout as C.SW
				uint32_t imm = (((raw16 >> 5) & 0x1) << 6) | (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 6) & 0x1) << 2);
				instr.mnemonic = "C.FSW";
				instr.ext = Extension::F;
				instr.opcode = 0b0100111; instr.funct3 = 0b010; instr.fp_double = false;
				instr.rs1 = creg_prime(raw16, 7); instr.rs2 = creg_prime(raw16, 2); instr.imm = (int64_t)imm;
			} else {
				instr.ext = Extension::ILLEGAL;
			}
			break;
		}
		case 0b100: { // Zcb: C.LBU/C.LHU/C.LH/C.SB/C.SH -- the byte and halfword
			// accesses RVC otherwise lacks. Expanded to their standard LBU/LHU/
			// LH/SB/SH equivalents like every other compressed encoding, so they
			// flow through exec_32I unchanged.
			uint8_t sub = (raw16 >> 10) & 0x3;   // funct6's low two bits pick the operation
			uint8_t b6 = (raw16 >> 6) & 0x1;
			uint8_t b5 = (raw16 >> 5) & 0x1;
			uint8_t rs1 = creg_prime(raw16, 7);
			uint8_t rd_rs2 = creg_prime(raw16, 2);
			switch (sub) {
			case 0b00: // C.LBU -> LBU rd', off(rs1'); off[1:0] = {b5, b6}
				instr.mnemonic = "C.LBU";
				instr.opcode = 0b0000011; instr.funct3 = 0b100;
				instr.rd = rd_rs2; instr.rs1 = rs1;
				instr.imm = (int64_t)((b5 << 1) | b6);
				break;
			case 0b01: // b6 picks C.LH (1) vs C.LHU (0); off[1] = b5, always halfword-aligned
				instr.mnemonic = b6 ? "C.LH" : "C.LHU";
				instr.opcode = 0b0000011; instr.funct3 = b6 ? 0b001 : 0b101;
				instr.rd = rd_rs2; instr.rs1 = rs1;
				instr.imm = (int64_t)(b5 << 1);
				break;
			case 0b10: // C.SB -> SB rs2', off(rs1')
				instr.mnemonic = "C.SB";
				instr.opcode = 0b0100011; instr.funct3 = 0b000;
				instr.rs1 = rs1; instr.rs2 = rd_rs2;
				instr.imm = (int64_t)((b5 << 1) | b6);
				break;
			default:  // 0b11 -- C.SH
				instr.mnemonic = "C.SH";
				instr.opcode = 0b0100011; instr.funct3 = 0b001;
				instr.rs1 = rs1; instr.rs2 = rd_rs2;
				instr.imm = (int64_t)(b5 << 1);
				break;
			}
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
		case 0b001: { // RV64: C.ADDIW (same imm layout as C.ADDI). RV32: this same slot is C.JAL instead
			if (Extensions.XLEN64) {
				int32_t imm = (int32_t)(((raw16 >> 2) & 0x1F) | (((raw16 >> 12) & 0x1) << 5));
				if (imm & 0x20) imm |= ~0x3F;
				uint8_t rd = creg_full(raw16, 7);
				instr.mnemonic = "C.ADDIW";
				instr.opcode = 0b0011011; instr.funct3 = 0b000; instr.word_op = true;
				instr.rd = rd; instr.rs1 = rd; instr.imm = imm;
			} else { // C.JAL -> JAL x1, imm (CJ-format, same layout as C.J below)
				int32_t imm = (int32_t)(((raw16 >> 3) & 0x7) << 1 | ((raw16 >> 11) & 0x1) << 4 | ((raw16 >> 2) & 0x1) << 5
				            | ((raw16 >> 7) & 0x1) << 6 | ((raw16 >> 6) & 0x1) << 7 | ((raw16 >> 9) & 0x3) << 8
				            | ((raw16 >> 8) & 0x1) << 10 | ((raw16 >> 12) & 0x1) << 11);
				if (imm & 0x800) imm |= ~0xFFF;
				instr.mnemonic = "C.JAL";
				instr.opcode = 0b1101111; instr.rd = 1; instr.imm = imm;
			}
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
			} else if ((raw16 >> 12) & 0x1) { // funct1==1 -> C.SUBW/C.ADDW (RV64 only; this whole slot is reserved on RV32)
				uint8_t rs2 = creg_prime(raw16, 2);
				uint8_t funct2 = (raw16 >> 5) & 0x3;
				if (Extensions.XLEN64 && (funct2 == 0b00 || funct2 == 0b01)) {
					bool is_subw = (funct2 == 0b00);
					instr.mnemonic = is_subw ? "C.SUBW" : "C.ADDW";
					instr.opcode = 0b0111011; instr.funct3 = 0b000; instr.word_op = true;
					instr.funct7 = is_subw ? 0b0100000 : 0b0000000;
					instr.rd = rd; instr.rs1 = rd; instr.rs2 = rs2;
				} else if (funct2 == 0b10) { // Zcb: C.MUL rd',rd',rs2' -> MUL
					instr.ext = Extension::M;
					instr.mnemonic = "C.MUL";
					instr.opcode = 0b0110011; instr.funct3 = 0b000; instr.funct7 = 0b0000001;
					instr.rd = rd; instr.rs1 = rd; instr.rs2 = rs2;
				} else if (funct2 != 0b11) {
					// Only reachable on RV32, where the SUBW/ADDW slot is reserved.
					instr.ext = Extension::ILLEGAL;
				} else { // funct2 == 0b11 -- Zcb's unary family, selected by bits[4:2]
					// Each expands to the standard instruction it is defined as an
					// alias for; the two that are plain masking (zext.b, not) stay in
					// base I, the rest borrow Zbb/Zba and gate on those accordingly.
					switch ((raw16 >> 2) & 0x7) {
					case 0b000: // C.ZEXT.B -> ANDI rd,rd,255
						instr.mnemonic = "C.ZEXT.B";
						instr.opcode = 0b0010011; instr.funct3 = 0b111;
						instr.rd = rd; instr.rs1 = rd; instr.imm = 255;
						break;
					case 0b001: // C.SEXT.B -> Zbb sext.b
						instr.ext = Extension::ZBB; instr.mnemonic = "C.SEXT.B";
						instr.opcode = 0b0010011; instr.funct3 = 0b001; instr.funct7 = 0b0110000;
						instr.rd = rd; instr.rs1 = rd; instr.rs2 = 4; instr.imm = 4;
						break;
					case 0b010: // C.ZEXT.H -> Zbb zext.h (OP-32 form on RV64)
						instr.ext = Extension::ZBB; instr.mnemonic = "C.ZEXT.H";
						instr.opcode = 0b0111011; instr.funct3 = 0b100; instr.funct7 = 0b0000100;
						instr.rd = rd; instr.rs1 = rd; instr.rs2 = 0; instr.word_op = true;
						break;
					case 0b011: // C.SEXT.H -> Zbb sext.h
						instr.ext = Extension::ZBB; instr.mnemonic = "C.SEXT.H";
						instr.opcode = 0b0010011; instr.funct3 = 0b001; instr.funct7 = 0b0110000;
						instr.rd = rd; instr.rs1 = rd; instr.rs2 = 5; instr.imm = 5;
						break;
					case 0b100: // C.ZEXT.W -> Zba add.uw rd,rd,x0 (RV64 only)
						if (!Extensions.XLEN64) { instr.ext = Extension::ILLEGAL; break; }
						instr.ext = Extension::ZBA; instr.mnemonic = "C.ZEXT.W";
						instr.opcode = 0b0111011; instr.funct3 = 0b000; instr.funct7 = 0b0000100;
						instr.rd = rd; instr.rs1 = rd; instr.rs2 = 0; instr.word_op = true;
						break;
					case 0b101: // C.NOT -> XORI rd,rd,-1
						instr.mnemonic = "C.NOT";
						instr.opcode = 0b0010011; instr.funct3 = 0b100;
						instr.rd = rd; instr.rs1 = rd; instr.imm = -1;
						break;
					default:
						instr.ext = Extension::ILLEGAL; // 110/111 reserved
						break;
					}
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
		case 0b001: { // C.FLDSP (needs D) -> FLD rd, imm(x2) -- rd is a full 5-bit F register, no x0-style restriction
			if (!Extensions.D) { instr.ext = Extension::ILLEGAL; break; }
			uint32_t imm = (((raw16 >> 5) & 0x3) << 3) | (((raw16 >> 12) & 0x1) << 5) | (((raw16 >> 2) & 0x7) << 6);
			instr.mnemonic = "C.FLDSP";
			instr.ext = Extension::D;
			instr.opcode = 0b0000111; instr.funct3 = 0b011; instr.fp_double = true;
			instr.rd = creg_full(raw16, 7); instr.rs1 = 2; instr.imm = (int64_t)imm;
			break;
		}
		case 0b010: { // C.LWSP rd,imm -> LW rd, imm(x2)
			uint32_t imm = (((raw16 >> 4) & 0x7) << 2) | (((raw16 >> 12) & 0x1) << 5) | (((raw16 >> 2) & 0x3) << 6);
			instr.mnemonic = "C.LWSP";
			instr.opcode = 0b0000011; instr.funct3 = 0b010;
			instr.rd = creg_full(raw16, 7); instr.rs1 = 2; instr.imm = (int64_t)imm;
			break;
		}
		case 0b011: { // RV64: C.LDSP -> LD rd, imm(x2). RV32: this slot is C.FLWSP instead (needs F)
			if (Extensions.XLEN64) {
				uint32_t imm = (((raw16 >> 5) & 0x3) << 3) | (((raw16 >> 12) & 0x1) << 5) | (((raw16 >> 2) & 0x7) << 6);
				instr.mnemonic = "C.LDSP";
				instr.opcode = 0b0000011; instr.funct3 = 0b011;
				instr.rd = creg_full(raw16, 7); instr.rs1 = 2; instr.imm = (int64_t)imm;
			} else if (Extensions.F) { // same imm layout as C.LWSP
				uint32_t imm = (((raw16 >> 4) & 0x7) << 2) | (((raw16 >> 12) & 0x1) << 5) | (((raw16 >> 2) & 0x3) << 6);
				instr.mnemonic = "C.FLWSP";
				instr.ext = Extension::F;
				instr.opcode = 0b0000111; instr.funct3 = 0b010; instr.fp_double = false;
				instr.rd = creg_full(raw16, 7); instr.rs1 = 2; instr.imm = (int64_t)imm;
			} else {
				instr.ext = Extension::ILLEGAL;
			}
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
		case 0b101: { // C.FSDSP (needs D) -> FSD rs2, imm(x2)
			if (!Extensions.D) { instr.ext = Extension::ILLEGAL; break; }
			uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 7) & 0x7) << 6);
			instr.mnemonic = "C.FSDSP";
			instr.ext = Extension::D;
			instr.opcode = 0b0100111; instr.funct3 = 0b011; instr.fp_double = true;
			instr.rs1 = 2; instr.rs2 = creg_full(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		case 0b110: { // C.SWSP rs2,imm -> SW rs2, imm(x2)
			uint32_t imm = (((raw16 >> 9) & 0xF) << 2) | (((raw16 >> 7) & 0x3) << 6);
			instr.mnemonic = "C.SWSP";
			instr.opcode = 0b0100011; instr.funct3 = 0b010;
			instr.rs1 = 2; instr.rs2 = creg_full(raw16, 2); instr.imm = (int64_t)imm;
			break;
		}
		case 0b111: { // RV64: C.SDSP -> SD rs2, imm(x2). RV32: this slot is C.FSWSP instead (needs F)
			if (Extensions.XLEN64) {
				uint32_t imm = (((raw16 >> 10) & 0x7) << 3) | (((raw16 >> 7) & 0x7) << 6);
				instr.mnemonic = "C.SDSP";
				instr.opcode = 0b0100011; instr.funct3 = 0b011;
				instr.rs1 = 2; instr.rs2 = creg_full(raw16, 2); instr.imm = (int64_t)imm;
			} else if (Extensions.F) { // same imm layout as C.SWSP
				uint32_t imm = (((raw16 >> 9) & 0xF) << 2) | (((raw16 >> 7) & 0x3) << 6);
				instr.mnemonic = "C.FSWSP";
				instr.ext = Extension::F;
				instr.opcode = 0b0100111; instr.funct3 = 0b010; instr.fp_double = false;
				instr.rs1 = 2; instr.rs2 = creg_full(raw16, 2); instr.imm = (int64_t)imm;
			} else {
				instr.ext = Extension::ILLEGAL;
			}
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
