// A extension: atomic memory operations (LR/SC/AMO*). RiscvCore's only
// piece of cross-instruction state (the LR/SC reservation) is exclusively
// used here, so the constructor that initializes it lives in this file too.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"

DecodedInstruction Decoder::decode_a(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::A;
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

	// funct7[6:2] selects the op, funct3 selects width (010=.W, 011=.D), funct7[1:0] are aq/rl
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
	if (instr.op_64 && !Extensions.XLEN64) instr.ext = Extension::ILLEGAL; // .D forms don't exist on RV32

	return instr;
}

RiscvCore::RiscvCore() : reservation_valid(false), reservation_addr(0)
{
}

void RiscvCore::exec_32A(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();
	uint64_t addr = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);
	uint8_t amo_op = instr.funct7 >> 2;
	bool is64 = instr.op_64;

	// LR needs R, SC needs W, every other AMO needs both per spec -- see
	// AccessType::Amo. Note the reservation itself is still tracked by
	// virtual address (not paddr): that's fine since nothing here models
	// a second hart or address-space switch that could alias two virtual
	// addresses onto the same physical reservation mid-sequence.
	AccessType amo_access = (amo_op == 0b00010) ? AccessType::Load
	                       : (amo_op == 0b00011) ? AccessType::Store
	                                              : AccessType::Amo;
	uint64_t paddr;
	if (!translate_or_trap(regs, mem, addr, amo_access, paddr)) return;

	if (amo_op == 0b00011) { // SC.W/SC.D
		if (reservation_valid && reservation_addr == addr) {
			if (is64) mem.write64(paddr, rs2_val);
			else mem.write32(paddr, (uint32_t)rs2_val);
			regs.write_x(instr.rd, 0); // success
		} else {
			regs.write_x(instr.rd, 1); // failure
		}
		reservation_valid = false;
		regs.set_pc(pc + instr.length);
		return;
	}

	// Pre-image, already sign-extended for the .W case -- AMO*.W and LR.W
	// both return the loaded value sign-extended to 64 bits per spec.
	uint64_t loaded = is64 ? mem.read64(paddr) : sext32(mem.read32(paddr));

	if (amo_op == 0b00010) { // LR.W/LR.D
		reservation_valid = true;
		reservation_addr = addr;
		regs.write_x(instr.rd, loaded);
		regs.set_pc(pc + instr.length);
		return;
	}

	if (is64) {
		int64_t loaded_s = (int64_t)loaded, rs2_s = (int64_t)rs2_val;
		uint64_t result = loaded;
		switch (amo_op) {
		case 0b00001: result = rs2_val; break;                                          // AMOSWAP.D
		case 0b00000: result = loaded + rs2_val; break;                                 // AMOADD.D
		case 0b00100: result = loaded ^ rs2_val; break;                                 // AMOXOR.D
		case 0b01100: result = loaded & rs2_val; break;                                 // AMOAND.D
		case 0b01000: result = loaded | rs2_val; break;                                 // AMOOR.D
		case 0b10000: result = (loaded_s < rs2_s) ? loaded : rs2_val; break;             // AMOMIN.D
		case 0b10100: result = (loaded_s > rs2_s) ? loaded : rs2_val; break;             // AMOMAX.D
		case 0b11000: result = (loaded < rs2_val) ? loaded : rs2_val; break;             // AMOMINU.D
		case 0b11100: result = (loaded > rs2_val) ? loaded : rs2_val; break;             // AMOMAXU.D
		default: break;
		}
		mem.write64(paddr, result);
	} else {
		uint32_t loaded32 = (uint32_t)loaded, rs2_32 = (uint32_t)rs2_val;
		int32_t loaded_s = (int32_t)loaded32, rs2_s = (int32_t)rs2_32;
		uint32_t result32 = loaded32;
		switch (amo_op) {
		case 0b00001: result32 = rs2_32; break;                                               // AMOSWAP.W
		case 0b00000: result32 = loaded32 + rs2_32; break;                                    // AMOADD.W
		case 0b00100: result32 = loaded32 ^ rs2_32; break;                                    // AMOXOR.W
		case 0b01100: result32 = loaded32 & rs2_32; break;                                    // AMOAND.W
		case 0b01000: result32 = loaded32 | rs2_32; break;                                    // AMOOR.W
		case 0b10000: result32 = (loaded_s < rs2_s) ? loaded32 : rs2_32; break;                // AMOMIN.W
		case 0b10100: result32 = (loaded_s > rs2_s) ? loaded32 : rs2_32; break;                // AMOMAX.W
		case 0b11000: result32 = (loaded32 < rs2_32) ? loaded32 : rs2_32; break;               // AMOMINU.W
		case 0b11100: result32 = (loaded32 > rs2_32) ? loaded32 : rs2_32; break;               // AMOMAXU.W
		default: break;
		}
		mem.write32(paddr, result32);
	}

	regs.write_x(instr.rd, loaded); // rd gets the pre-op value for every real AMO op
	regs.set_pc(pc + instr.length);
}
