#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>
#include <climits>

namespace {
// M-mode CSR addresses actually given meaning by exec_32ZICSR/enter_trap.
// Anything else (mscratch, misa, mhartid, ...) is still fully readable/
// writable -- Registers::csr[] backs all 4096 addresses generically -- it
// just has no side effects, which is correct for those.
constexpr uint16_t CSR_MSTATUS = 0x300;
constexpr uint16_t CSR_MTVEC   = 0x305;
constexpr uint16_t CSR_MEPC    = 0x341;
constexpr uint16_t CSR_MCAUSE  = 0x342;
constexpr uint16_t CSR_MTVAL   = 0x343;

constexpr uint32_t MSTATUS_MIE  = 1u << 3;
constexpr uint32_t MSTATUS_MPIE = 1u << 7;

constexpr uint32_t CAUSE_BREAKPOINT   = 3;
constexpr uint32_t CAUSE_ECALL_FROM_M = 11;
}

RiscvCore::RiscvCore() : reservation_valid(false), reservation_addr(0)
{
}

void RiscvCore::exec_32I(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint32_t pc = regs.get_pc();
	uint32_t next_pc = pc + instr.length;

	uint32_t rs1_val = regs.read_x(instr.rs1);
	uint32_t rs2_val = regs.read_x(instr.rs2);
	int32_t rs1_s = (int32_t)rs1_val;
	int32_t rs2_s = (int32_t)rs2_val;

	switch (instr.opcode) {
	case 0b0110111: // LUI
		regs.write_x(instr.rd, (uint32_t)instr.imm);
		break;

	case 0b0010111: // AUIPC
		regs.write_x(instr.rd, pc + (uint32_t)instr.imm);
		break;

	case 0b1101111: // JAL
		regs.write_x(instr.rd, next_pc);
		next_pc = pc + (uint32_t)instr.imm;
		break;

	case 0b1100111: // JALR
		{
			uint32_t target = (rs1_val + (uint32_t)instr.imm) & ~1u;
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
		if (taken) next_pc = pc + (uint32_t)instr.imm;
		break;
	}

	case 0b0000011: { // Load
		uint32_t addr = rs1_val + (uint32_t)instr.imm;
		uint32_t val = 0;
		switch (instr.funct3) {
		case 0b000: val = (uint32_t)(int32_t)(int8_t)mem.read8(addr);   break; // LB
		case 0b001: val = (uint32_t)(int32_t)(int16_t)mem.read16(addr); break; // LH
		case 0b010: val = mem.read32(addr);                             break; // LW
		case 0b100: val = mem.read8(addr);                              break; // LBU
		case 0b101: val = mem.read16(addr);                             break; // LHU
		}
		regs.write_x(instr.rd, val);
		break;
	}

	case 0b0100011: { // Store
		uint32_t addr = rs1_val + (uint32_t)instr.imm;
		switch (instr.funct3) {
		case 0b000: // SB
			mem.write8(addr, (uint8_t)rs2_val);
			break;
		case 0b001: // SH
			mem.write8(addr, (uint8_t)(rs2_val & 0xFF));
			mem.write8(addr + 1, (uint8_t)((rs2_val >> 8) & 0xFF));
			break;
		case 0b010: // SW
			mem.write32(addr, rs2_val);
			break;
		}
		break;
	}

	case 0b0010011: { // OP-IMM
		uint32_t result = 0;
		switch (instr.funct3) {
		case 0b000: result = rs1_val + (uint32_t)instr.imm; break; // ADDI
		case 0b010: result = (rs1_s < instr.imm) ? 1 : 0; break;   // SLTI
		case 0b011: result = (rs1_val < (uint32_t)instr.imm) ? 1 : 0; break; // SLTIU
		case 0b100: result = rs1_val ^ (uint32_t)instr.imm; break; // XORI
		case 0b110: result = rs1_val | (uint32_t)instr.imm; break; // ORI
		case 0b111: result = rs1_val & (uint32_t)instr.imm; break; // ANDI
		case 0b001: result = rs1_val << (instr.imm & 0x1F); break; // SLLI (imm holds shamt)
		case 0b101:
			result = (instr.funct7 == 0b0100000)
			       ? (uint32_t)(rs1_s >> (instr.imm & 0x1F))  // SRAI
			       : (rs1_val >> (instr.imm & 0x1F));          // SRLI
			break;
		}
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b0110011: { // OP (R-type, I-side only -- M-side goes through exec_32M)
		uint32_t result = 0;
		switch (instr.funct3) {
		case 0b000: result = (instr.funct7 == 0b0100000) ? (rs1_val - rs2_val) : (rs1_val + rs2_val); break; // SUB/ADD
		case 0b001: result = rs1_val << (rs2_val & 0x1F); break; // SLL
		case 0b010: result = (rs1_s < rs2_s) ? 1 : 0; break;     // SLT
		case 0b011: result = (rs1_val < rs2_val) ? 1 : 0; break; // SLTU
		case 0b100: result = rs1_val ^ rs2_val; break;           // XOR
		case 0b101:
			result = (instr.funct7 == 0b0100000)
			       ? (uint32_t)(rs1_s >> (rs2_val & 0x1F))  // SRA
			       : (rs1_val >> (rs2_val & 0x1F));          // SRL
			break;
		case 0b110: result = rs1_val | rs2_val; break; // OR
		case 0b111: result = rs1_val & rs2_val; break; // AND
		}
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b0001111: // FENCE / FENCE.I -- NOP, no icache/reordering to manage here
		break;

	default:
		break; // decoder gates unrecognized opcodes to illegal before this is ever called
	}

	regs.set_pc(next_pc);
}

void RiscvCore::exec_32M(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint32_t pc = regs.get_pc();

	uint32_t rs1_val = regs.read_x(instr.rs1);
	uint32_t rs2_val = regs.read_x(instr.rs2);
	int32_t rs1_s = (int32_t)rs1_val;
	int32_t rs2_s = (int32_t)rs2_val;

	uint32_t result = 0;
	switch (instr.funct3) {
	case 0b000: // MUL
		result = rs1_val * rs2_val;
		break;
	case 0b001: { // MULH (signed x signed, upper 32 bits)
		int64_t prod = (int64_t)rs1_s * (int64_t)rs2_s;
		result = (uint32_t)((uint64_t)prod >> 32);
		break;
	}
	case 0b010: { // MULHSU (signed x unsigned)
		int64_t prod = (int64_t)rs1_s * (int64_t)(uint64_t)rs2_val;
		result = (uint32_t)((uint64_t)prod >> 32);
		break;
	}
	case 0b011: { // MULHU (unsigned x unsigned)
		uint64_t prod = (uint64_t)rs1_val * (uint64_t)rs2_val;
		result = (uint32_t)(prod >> 32);
		break;
	}
	case 0b100: // DIV
		if (rs2_s == 0) result = 0xFFFFFFFF;
		else if (rs1_s == INT32_MIN && rs2_s == -1) result = (uint32_t)INT32_MIN;
		else result = (uint32_t)(rs1_s / rs2_s);
		break;
	case 0b101: // DIVU
		result = (rs2_val == 0) ? 0xFFFFFFFF : (rs1_val / rs2_val);
		break;
	case 0b110: // REM
		if (rs2_s == 0) result = rs1_val;
		else if (rs1_s == INT32_MIN && rs2_s == -1) result = 0;
		else result = (uint32_t)(rs1_s % rs2_s);
		break;
	case 0b111: // REMU
		result = (rs2_val == 0) ? rs1_val : (rs1_val % rs2_val);
		break;
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}

void RiscvCore::exec_32A(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint32_t pc = regs.get_pc();
	uint32_t addr = regs.read_x(instr.rs1);
	uint32_t rs2_val = regs.read_x(instr.rs2);
	uint8_t amo_op = instr.funct7 >> 2;

	if (amo_op == 0b00011) { // SC.W
		if (reservation_valid && reservation_addr == addr) {
			mem.write32(addr, rs2_val);
			regs.write_x(instr.rd, 0); // success
		} else {
			regs.write_x(instr.rd, 1); // failure
		}
		reservation_valid = false;
		regs.set_pc(pc + instr.length);
		return;
	}

	uint32_t loaded = mem.read32(addr);

	if (amo_op == 0b00010) { // LR.W
		reservation_valid = true;
		reservation_addr = addr;
		regs.write_x(instr.rd, loaded);
		regs.set_pc(pc + instr.length);
		return;
	}

	uint32_t result = loaded;
	switch (amo_op) {
	case 0b00001: result = rs2_val; break;                                                     // AMOSWAP.W
	case 0b00000: result = loaded + rs2_val; break;                                            // AMOADD.W
	case 0b00100: result = loaded ^ rs2_val; break;                                             // AMOXOR.W
	case 0b01100: result = loaded & rs2_val; break;                                             // AMOAND.W
	case 0b01000: result = loaded | rs2_val; break;                                             // AMOOR.W
	case 0b10000: result = ((int32_t)loaded < (int32_t)rs2_val) ? loaded : rs2_val; break;      // AMOMIN.W
	case 0b10100: result = ((int32_t)loaded > (int32_t)rs2_val) ? loaded : rs2_val; break;      // AMOMAX.W
	case 0b11000: result = (loaded < rs2_val) ? loaded : rs2_val; break;                        // AMOMINU.W
	case 0b11100: result = (loaded > rs2_val) ? loaded : rs2_val; break;                        // AMOMAXU.W
	default: break;
	}

	mem.write32(addr, result);
	regs.write_x(instr.rd, loaded); // rd gets the pre-op value for every real AMO op
	regs.set_pc(pc + instr.length);
}

void RiscvCore::enter_trap(Registers &regs, uint32_t cause, uint32_t tval)
{
	uint32_t pc = regs.get_pc();
	regs.write_csr(CSR_MEPC, pc);
	regs.write_csr(CSR_MCAUSE, cause);
	regs.write_csr(CSR_MTVAL, tval);

	// Standard M-mode enable stacking: the current interrupt-enable bit is
	// saved to MPIE and cleared, so a handler doesn't get pre-empted by
	// itself; MRET reverses this.
	uint32_t mstatus = regs.read_csr(CSR_MSTATUS);
	mstatus = (mstatus & MSTATUS_MIE) ? (mstatus | MSTATUS_MPIE) : (mstatus & ~MSTATUS_MPIE);
	mstatus &= ~MSTATUS_MIE;
	regs.write_csr(CSR_MSTATUS, mstatus);

	// Direct mode only (mtvec[1:0] ignored) -- vectored mode's cause-based
	// offset only applies to interrupts, and this project has no interrupt
	// sources (no timer/external IRQ controller), only synchronous
	// exceptions, which always go to the base address regardless of mode.
	regs.set_pc(regs.read_csr(CSR_MTVEC) & ~0x3u);
}

void RiscvCore::exec_32ZICSR(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint32_t pc = regs.get_pc();

	if (instr.funct3 == 0) {
		// ECALL/EBREAK/MRET -- control transfer, not a CSR read/modify/write.
		// Illegal-instruction detection deliberately stays a separate,
		// unconditional debugger halt (see DoomSystem::step) rather than a
		// real trap here: nothing in this project sets up mtvec to actually
		// handle one, so routing illegal instructions through this same
		// path would just spin forever re-trapping instead of surfacing a
		// crash log.
		switch (instr.imm) {
		case 0x000: // ECALL
			enter_trap(regs, CAUSE_ECALL_FROM_M, 0);
			return;
		case 0x001: // EBREAK
			enter_trap(regs, CAUSE_BREAKPOINT, pc);
			return;
		case 0x302: { // MRET
			uint32_t mstatus = regs.read_csr(CSR_MSTATUS);
			mstatus = (mstatus & MSTATUS_MPIE) ? (mstatus | MSTATUS_MIE) : (mstatus & ~MSTATUS_MIE);
			mstatus |= MSTATUS_MPIE; // MPIE reset to 1 on return, per spec
			regs.write_csr(CSR_MSTATUS, mstatus);
			regs.set_pc(regs.read_csr(CSR_MEPC));
			return;
		}
		default:
			// Unimplemented privileged op (WFI, SFENCE.VMA, ...) -- not
			// expected without an OS; treated as a no-op like decode()'s
			// other unrecognized-but-enabled encodings.
			regs.set_pc(pc + instr.length);
			return;
		}
	}

	uint16_t csr = (uint16_t)instr.imm;
	uint32_t old = regs.read_csr(csr);
	// The *I forms (funct3 bit 2 set) use the rs1 field as a 5-bit
	// zero-extended immediate instead of a register number.
	uint32_t operand = (instr.funct3 & 0x4) ? instr.rs1 : regs.read_x(instr.rs1);

	uint32_t updated = old;
	switch (instr.funct3 & 0x3) {
	case 0b01: updated = operand; break; // CSRRW/CSRRWI -- always writes
	case 0b10: if (instr.rs1 != 0) updated = old | operand; break;  // CSRRS/CSRRSI -- rs1/uimm==0 means read-only
	case 0b11: if (instr.rs1 != 0) updated = old & ~operand; break; // CSRRC/CSRRCI
	}

	regs.write_csr(csr, updated);
	regs.write_x(instr.rd, old);
	regs.set_pc(pc + instr.length);
}
