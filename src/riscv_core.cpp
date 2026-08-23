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

constexpr uint64_t MSTATUS_MIE  = 1ull << 3;
constexpr uint64_t MSTATUS_MPIE = 1ull << 7;

constexpr uint64_t CAUSE_BREAKPOINT   = 3;
constexpr uint64_t CAUSE_ECALL_FROM_M = 11;

// sign-extend the low 32 bits of a 64-bit value to the full 64 -- the
// operation every *W-suffixed RV64 instruction ends with.
inline uint64_t sext32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }
}

RiscvCore::RiscvCore() : reservation_valid(false), reservation_addr(0)
{
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

void RiscvCore::exec_32M(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();

	uint64_t rs1_val = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);
	int64_t rs1_s = (int64_t)rs1_val;
	int64_t rs2_s = (int64_t)rs2_val;

	uint64_t result = 0;

	if (instr.word_op) {
		// MULW/DIVW/DIVUW/REMW/REMUW: operate on the low 32 bits, sign-
		// extend the 32-bit result to 64 -- including DIVUW/REMUW, which
		// despite being unsigned math still sign-extend per the W-suffix
		// convention.
		int32_t a = (int32_t)rs1_val, b = (int32_t)rs2_val;
		uint32_t ua = (uint32_t)rs1_val, ub = (uint32_t)rs2_val;
		uint32_t result32 = 0;
		switch (instr.funct3) {
		case 0b000: result32 = (uint32_t)(a * b); break; // MULW
		case 0b100: // DIVW
			if (b == 0) result32 = 0xFFFFFFFF;
			else if (a == INT32_MIN && b == -1) result32 = (uint32_t)INT32_MIN;
			else result32 = (uint32_t)(a / b);
			break;
		case 0b101: // DIVUW
			result32 = (ub == 0) ? 0xFFFFFFFF : (ua / ub);
			break;
		case 0b110: // REMW
			if (b == 0) result32 = (uint32_t)a;
			else if (a == INT32_MIN && b == -1) result32 = 0;
			else result32 = (uint32_t)(a % b);
			break;
		case 0b111: // REMUW
			result32 = (ub == 0) ? ua : (ua % ub);
			break;
		}
		result = sext32(result32);
	} else {
		switch (instr.funct3) {
		case 0b000: // MUL -- low 64 bits of the product, same regardless of signedness
			result = rs1_val * rs2_val;
			break;
		case 0b001: { // MULH (signed x signed, upper 64 bits of a 128-bit product)
			__int128 prod = (__int128)rs1_s * (__int128)rs2_s;
			result = (uint64_t)((unsigned __int128)prod >> 64);
			break;
		}
		case 0b010: { // MULHSU (signed x unsigned)
			__int128 prod = (__int128)rs1_s * (__int128)(unsigned __int128)rs2_val;
			result = (uint64_t)((unsigned __int128)prod >> 64);
			break;
		}
		case 0b011: { // MULHU (unsigned x unsigned)
			unsigned __int128 prod = (unsigned __int128)rs1_val * (unsigned __int128)rs2_val;
			result = (uint64_t)(prod >> 64);
			break;
		}
		case 0b100: // DIV
			if (rs2_s == 0) result = UINT64_MAX;
			else if (rs1_s == INT64_MIN && rs2_s == -1) result = (uint64_t)INT64_MIN;
			else result = (uint64_t)(rs1_s / rs2_s);
			break;
		case 0b101: // DIVU
			result = (rs2_val == 0) ? UINT64_MAX : (rs1_val / rs2_val);
			break;
		case 0b110: // REM
			if (rs2_s == 0) result = rs1_val;
			else if (rs1_s == INT64_MIN && rs2_s == -1) result = 0;
			else result = (uint64_t)(rs1_s % rs2_s);
			break;
		case 0b111: // REMU
			result = (rs2_val == 0) ? rs1_val : (rs1_val % rs2_val);
			break;
		}
	}

	regs.write_x(instr.rd, result);
	regs.set_pc(pc + instr.length);
}

void RiscvCore::exec_32A(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();
	uint64_t addr = regs.read_x(instr.rs1);
	uint64_t rs2_val = regs.read_x(instr.rs2);
	uint8_t amo_op = instr.funct7 >> 2;
	bool is64 = instr.op_64;

	if (amo_op == 0b00011) { // SC.W/SC.D
		if (reservation_valid && reservation_addr == addr) {
			if (is64) mem.write64(addr, rs2_val);
			else mem.write32(addr, (uint32_t)rs2_val);
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
	uint64_t loaded = is64 ? mem.read64(addr) : sext32(mem.read32(addr));

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
		mem.write64(addr, result);
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
		mem.write32(addr, result32);
	}

	regs.write_x(instr.rd, loaded); // rd gets the pre-op value for every real AMO op
	regs.set_pc(pc + instr.length);
}

void RiscvCore::enter_trap(Registers &regs, uint64_t cause, uint64_t tval)
{
	uint64_t pc = regs.get_pc();
	regs.write_csr(CSR_MEPC, pc);
	regs.write_csr(CSR_MCAUSE, cause);
	regs.write_csr(CSR_MTVAL, tval);

	// Standard M-mode enable stacking: the current interrupt-enable bit is
	// saved to MPIE and cleared, so a handler doesn't get pre-empted by
	// itself; MRET reverses this.
	uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
	mstatus = (mstatus & MSTATUS_MIE) ? (mstatus | MSTATUS_MPIE) : (mstatus & ~MSTATUS_MPIE);
	mstatus &= ~MSTATUS_MIE;
	regs.write_csr(CSR_MSTATUS, mstatus);

	// Direct mode only (mtvec[1:0] ignored) -- vectored mode's cause-based
	// offset only applies to interrupts, and this project has no interrupt
	// sources (no timer/external IRQ controller), only synchronous
	// exceptions, which always go to the base address regardless of mode.
	regs.set_pc(regs.read_csr(CSR_MTVEC) & ~0x3ull);
}

void RiscvCore::exec_32ZICSR(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	uint64_t pc = regs.get_pc();

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
			uint64_t mstatus = regs.read_csr(CSR_MSTATUS);
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
	uint64_t old = regs.read_csr(csr);
	// The *I forms (funct3 bit 2 set) use the rs1 field as a 5-bit
	// zero-extended immediate instead of a register number.
	uint64_t operand = (instr.funct3 & 0x4) ? instr.rs1 : regs.read_x(instr.rs1);

	uint64_t updated = old;
	switch (instr.funct3 & 0x3) {
	case 0b01: updated = operand; break; // CSRRW/CSRRWI -- always writes
	case 0b10: if (instr.rs1 != 0) updated = old | operand; break;  // CSRRS/CSRRSI -- rs1/uimm==0 means read-only
	case 0b11: if (instr.rs1 != 0) updated = old & ~operand; break; // CSRRC/CSRRCI
	}

	regs.write_csr(csr, updated);
	regs.write_x(instr.rd, old);
	regs.set_pc(pc + instr.length);
}
