#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "extensions.hpp"
#include <cstdint>
#include <climits>
#include <cfenv>
#include <cmath>
#include <cstring>

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

// ---- F/D support -------------------------------------------------------
//
// The FP register file (Registers::f[32]) is always 64 bits wide, whether
// the value it holds is single- or double-precision -- when D is present,
// a single-precision value is stored "NaN-boxed": the upper 32 bits are
// all 1s, so a value that ISN'T properly boxed (some bit pattern other
// than 0xFFFFFFFF up top) reads back as the canonical quiet NaN instead
// of whatever garbage was there, per spec. Raw bit reinterpretation goes
// through memcpy, not pointer casts, to stay clear of strict-aliasing UB.
inline uint32_t bits_from_f32(float f)  { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float    f32_from_bits(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline uint64_t bits_from_f64(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline double   f64_from_bits(uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

inline uint64_t box_f32(uint32_t bits) { return 0xFFFFFFFF00000000ull | bits; }
inline float unbox_f32(uint64_t boxed)
{
	uint32_t bits = ((boxed >> 32) == 0xFFFFFFFFull) ? (uint32_t)boxed : 0x7FC00000u;
	return f32_from_bits(bits);
}

inline float  read_f32_reg(Registers &r, int i) { return unbox_f32(bits_from_f64(r.read_f(i))); }
inline void   write_f32_reg(Registers &r, int i, float v) { r.write_f(i, f64_from_bits(box_f32(bits_from_f32(v)))); }

// RISC-V's rm field: 000=RNE,001=RTZ,010=RDN,011=RUP,100=RMM,111=dynamic
// (use fcsr's frm). x86/host <cfenv> has no native "round to nearest,
// ties away from zero" (RMM) -- approximated as round-to-nearest-even,
// a defensible simplification since RMM is rarely used in practice and
// only differs from RNE on exact halfway ties.
inline int host_round_mode(uint8_t rm, uint8_t frm)
{
	uint8_t mode = (rm == 0b111) ? frm : rm;
	switch (mode) {
	case 0b001: return FE_TOWARDZERO; // RTZ
	case 0b010: return FE_DOWNWARD;   // RDN
	case 0b011: return FE_UPWARD;     // RUP
	default:    return FE_TONEAREST;  // RNE, RMM (approximated), reserved encodings
	}
}

// fflags bit positions (NV/DZ/OF/UF/NX), mapped from whatever the host
// FPU actually signaled during the op just performed.
inline uint8_t collect_fflags()
{
	int ex = std::fetestexcept(FE_ALL_EXCEPT);
	uint8_t flags = 0;
	if (ex & FE_INVALID)   flags |= 0x10;
	if (ex & FE_DIVBYZERO) flags |= 0x08;
	if (ex & FE_OVERFLOW)  flags |= 0x04;
	if (ex & FE_UNDERFLOW) flags |= 0x02;
	if (ex & FE_INEXACT)   flags |= 0x01;
	return flags;
}

// Shared "set rounding mode, compute, restore, collect exceptions" shape
// used by every rounded arithmetic op -- one template instead of writing
// this out separately for float and double at each of add/sub/mul/div/
// sqrt/fma (8+ call sites).
template <typename T>
T fp_binop(T a, T b, char op, uint8_t rm, Registers &regs)
{
	std::feclearexcept(FE_ALL_EXCEPT);
	int old_round = std::fegetround();
	std::fesetround(host_round_mode(rm, regs.get_frm()));
	T result = 0;
	switch (op) {
	case '+': result = a + b; break;
	case '-': result = a - b; break;
	case '*': result = a * b; break;
	case '/': result = a / b; break;
	}
	std::fesetround(old_round);
	regs.or_fflags(collect_fflags());
	return result;
}

template <typename T>
T fp_sqrt(T a, uint8_t rm, Registers &regs)
{
	std::feclearexcept(FE_ALL_EXCEPT);
	int old_round = std::fegetround();
	std::fesetround(host_round_mode(rm, regs.get_frm()));
	T result = std::sqrt(a);
	std::fesetround(old_round);
	regs.or_fflags(collect_fflags());
	return result;
}

template <typename T>
T fp_fma(T a, T b, T c, uint8_t rm, Registers &regs)
{
	std::feclearexcept(FE_ALL_EXCEPT);
	int old_round = std::fegetround();
	std::fesetround(host_round_mode(rm, regs.get_frm()));
	T result = std::fma(a, b, c);
	std::fesetround(old_round);
	regs.or_fflags(collect_fflags());
	return result;
}

// FEQ/FLT/FLE: a NaN operand compares false and sets NV. Spec actually
// distinguishes signaling from quiet NaN (only signaling NaN forces NV
// for FEQ; both do for FLT/FLE) -- simplified here to "any NaN sets NV",
// which can set the flag slightly more often than strict compliance but
// never changes the returned comparison result.
template <typename T>
uint64_t fcompare(T a, T b, uint8_t funct3, Registers &regs)
{
	if (std::isnan(a) || std::isnan(b)) {
		regs.or_fflags(0x10);
		return 0;
	}
	switch (funct3) {
	case 0b010: return a == b ? 1 : 0; // FEQ
	case 0b001: return a < b  ? 1 : 0; // FLT
	default:    return a <= b ? 1 : 0; // FLE
	}
}

// FMIN/FMAX: canonical NaN if both inputs are NaN, the non-NaN operand if
// only one is -- std::fmin/fmax already implement exactly that. Same NaN-
// signaling simplification as fcompare above.
template <typename T>
T fminmax(T a, T b, bool is_max, Registers &regs)
{
	if (std::isnan(a) || std::isnan(b)) regs.or_fflags(0x10);
	return is_max ? std::fmax(a, b) : std::fmin(a, b);
}

// FSGNJ/FSGNJN/FSGNJX: pure bit manipulation (copy rs1's magnitude, take
// rs2's sign bit, possibly negated/XORed in) -- no rounding, no exceptions.
inline float fsgnj_f32(float a, float b, uint8_t funct3)
{
	uint32_t ab = bits_from_f32(a), bb = bits_from_f32(b), sign = 0x80000000u, r;
	switch (funct3) {
	case 0b000: r = (ab & ~sign) | (bb & sign); break;
	case 0b001: r = (ab & ~sign) | (~bb & sign); break;
	default:    r = ab ^ (bb & sign); break;
	}
	return f32_from_bits(r);
}
inline double fsgnj_f64(double a, double b, uint8_t funct3)
{
	uint64_t ab = bits_from_f64(a), bb = bits_from_f64(b), sign = 0x8000000000000000ull, r;
	switch (funct3) {
	case 0b000: r = (ab & ~sign) | (bb & sign); break;
	case 0b001: r = (ab & ~sign) | (~bb & sign); break;
	default:    r = ab ^ (bb & sign); break;
	}
	return f64_from_bits(r);
}

// FCLASS: 10-bit category mask (bit0=-inf ... bit9=quiet NaN). Signaling
// vs quiet NaN is told apart by the mantissa's MSB, the usual convention.
template <typename T>
uint64_t fclassify(T v)
{
	if (std::isnan(v)) {
		bool quiet;
		if constexpr (sizeof(T) == 4) { quiet = (bits_from_f32((float)v) >> 22) & 1; }
		else { quiet = (bits_from_f64((double)v) >> 51) & 1; }
		return quiet ? (1ull << 9) : (1ull << 8);
	}
	bool neg = std::signbit(v);
	if (std::isinf(v)) return neg ? (1ull << 0) : (1ull << 7);
	if (v == 0) return neg ? (1ull << 3) : (1ull << 4);
	if (std::fpclassify(v) == FP_SUBNORMAL) return neg ? (1ull << 2) : (1ull << 5);
	return neg ? (1ull << 1) : (1ull << 6);
}

// Float/double -> integer conversions. Out-of-range and NaN clamp to the
// target type's max/min (NaN -> max) and set NV, matching FCVT.*'s spec'd
// "invalid" behavior instead of relying on UB from an out-of-range cast.
inline int32_t fcvt_to_i32(double v, Registers &regs)
{
	if (std::isnan(v) || v >= 2147483648.0) { regs.or_fflags(0x10); return INT32_MAX; }
	if (v < -2147483648.0) { regs.or_fflags(0x10); return INT32_MIN; }
	return (int32_t)std::llrint(v);
}
inline uint32_t fcvt_to_u32(double v, Registers &regs)
{
	if (std::isnan(v) || v >= 4294967296.0) { regs.or_fflags(0x10); return 0xFFFFFFFFu; }
	if (v < 0.0) { regs.or_fflags(0x10); return 0; }
	return (uint32_t)std::llrint(v);
}
inline int64_t fcvt_to_i64(double v, Registers &regs)
{
	if (std::isnan(v) || v >= 9223372036854775808.0) { regs.or_fflags(0x10); return INT64_MAX; }
	if (v < -9223372036854775808.0) { regs.or_fflags(0x10); return INT64_MIN; }
	return std::llrint(v);
}
inline uint64_t fcvt_to_u64(double v, Registers &regs)
{
	// A double's 52-bit mantissa can't exactly represent every uint64_t
	// near the top of its range, but that's an inherent precision limit
	// of using double as the common conversion path, not a correctness
	// bug -- FCVT.LU.* is not something Doom-adjacent code is expected
	// to lean on for exact huge-integer round-tripping.
	if (std::isnan(v) || v >= 18446744073709551616.0) { regs.or_fflags(0x10); return UINT64_MAX; }
	if (v < 0.0) { regs.or_fflags(0x10); return 0; }
	return (uint64_t)std::llrint(v);
}
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
	} else if (!Extensions.XLEN64) {
		// RV32: M's base instructions are inherently 32-bit -- there's no
		// separate OP-32 opcode in RV32 at all -- so this is the same
		// "compute low 32, sign-extend" shape as the word_op branch above,
		// just covering the full 8-instruction set. RV64's W-suffix group
		// only has 5 (no MULH/MULHSU/MULHU): a 32x32 multiply's high half
		// is redundant to expose separately once XLEN is already 64, since
		// the full 64-bit product is already available from plain MUL.
		int32_t a = (int32_t)rs1_val, b = (int32_t)rs2_val;
		uint32_t ua = (uint32_t)rs1_val, ub = (uint32_t)rs2_val;
		uint32_t result32 = 0;
		switch (instr.funct3) {
		case 0b000: result32 = (uint32_t)(a * b); break; // MUL
		case 0b001: result32 = (uint32_t)(((int64_t)a * (int64_t)b) >> 32); break; // MULH
		case 0b010: result32 = (uint32_t)(((int64_t)a * (int64_t)ub) >> 32); break; // MULHSU
		case 0b011: result32 = (uint32_t)(((uint64_t)ua * (uint64_t)ub) >> 32); break; // MULHU
		case 0b100: // DIV
			if (b == 0) result32 = 0xFFFFFFFF;
			else if (a == INT32_MIN && b == -1) result32 = (uint32_t)INT32_MIN;
			else result32 = (uint32_t)(a / b);
			break;
		case 0b101: // DIVU
			result32 = (ub == 0) ? 0xFFFFFFFF : (ua / ub);
			break;
		case 0b110: // REM
			if (b == 0) result32 = (uint32_t)a;
			else if (a == INT32_MIN && b == -1) result32 = 0;
			else result32 = (uint32_t)(a % b);
			break;
		case 0b111: // REMU
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
	// fflags(0x001)/frm(0x002)/fcsr(0x003) live in Registers' dedicated
	// fields, not the generic csr[] array -- fflags needs OR-accumulate
	// semantics from FP ops that a plain array slot can't express, and
	// fcsr is just those two fields packed together (frm in bits[7:5],
	// fflags in bits[4:0]).
	uint64_t old;
	if (csr == 0x001) old = regs.get_fflags();
	else if (csr == 0x002) old = regs.get_frm();
	else if (csr == 0x003) old = ((uint64_t)regs.get_frm() << 5) | regs.get_fflags();
	else old = regs.read_csr(csr);

	// The *I forms (funct3 bit 2 set) use the rs1 field as a 5-bit
	// zero-extended immediate instead of a register number.
	uint64_t operand = (instr.funct3 & 0x4) ? instr.rs1 : regs.read_x(instr.rs1);

	uint64_t updated = old;
	switch (instr.funct3 & 0x3) {
	case 0b01: updated = operand; break; // CSRRW/CSRRWI -- always writes
	case 0b10: if (instr.rs1 != 0) updated = old | operand; break;  // CSRRS/CSRRSI -- rs1/uimm==0 means read-only
	case 0b11: if (instr.rs1 != 0) updated = old & ~operand; break; // CSRRC/CSRRCI
	}

	if (csr == 0x001) regs.set_fflags((uint8_t)updated);
	else if (csr == 0x002) regs.set_frm((uint8_t)updated);
	else if (csr == 0x003) { regs.set_frm((uint8_t)(updated >> 5)); regs.set_fflags((uint8_t)updated); }
	else regs.write_csr(csr, updated);

	regs.write_x(instr.rd, old);
	regs.set_pc(pc + instr.length);
}

void RiscvCore::exec_FD(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	uint64_t pc = regs.get_pc();

	if (instr.opcode == 0b0000111) { // LOAD-FP: FLW/FLD -- rs1 is an integer base register
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		if (instr.fp_double) regs.write_f(instr.rd, f64_from_bits(mem.read64(addr)));
		else regs.write_f(instr.rd, f64_from_bits(box_f32(mem.read32(addr))));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b0100111) { // STORE-FP: FSW/FSD -- raw low bits, no unboxing/canonicalization needed
		uint64_t addr = regs.read_x(instr.rs1) + (uint64_t)instr.imm;
		if (instr.fp_double) mem.write64(addr, bits_from_f64(regs.read_f(instr.rs2)));
		else mem.write32(addr, (uint32_t)bits_from_f64(regs.read_f(instr.rs2)));
		regs.set_pc(pc + instr.length);
		return;
	}

	if (instr.opcode == 0b1000011 || instr.opcode == 0b1000111 ||
	    instr.opcode == 0b1001011 || instr.opcode == 0b1001111) { // FMADD/FMSUB/FNMSUB/FNMADD
		bool negate_c = (instr.opcode == 0b1000111 || instr.opcode == 0b1001111);
		bool negate_a = (instr.opcode == 0b1001011 || instr.opcode == 0b1001111);
		if (instr.fp_double) {
			double a = regs.read_f(instr.rs1), b = regs.read_f(instr.rs2), c = regs.read_f(instr.rs3);
			if (negate_a) a = -a;
			if (negate_c) c = -c;
			regs.write_f(instr.rd, fp_fma(a, b, c, instr.funct3, regs));
		} else {
			float a = read_f32_reg(regs, instr.rs1), b = read_f32_reg(regs, instr.rs2), c = read_f32_reg(regs, instr.rs3);
			if (negate_a) a = -a;
			if (negate_c) c = -c;
			write_f32_reg(regs, instr.rd, fp_fma(a, b, c, instr.funct3, regs));
		}
		regs.set_pc(pc + instr.length);
		return;
	}

	// Everything else is OP-FP (0b1010011); instr.funct7 re-selects the
	// exact operation the same way decode() did to pick its mnemonic.
	switch (instr.funct7) {
	case 0b0000000: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '+', instr.funct3, regs)); break; // FADD.S
	case 0b0000001: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '+', instr.funct3, regs)); break; // FADD.D
	case 0b0000100: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '-', instr.funct3, regs)); break; // FSUB.S
	case 0b0000101: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '-', instr.funct3, regs)); break; // FSUB.D
	case 0b0001000: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '*', instr.funct3, regs)); break; // FMUL.S
	case 0b0001001: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '*', instr.funct3, regs)); break; // FMUL.D
	case 0b0001100: write_f32_reg(regs, instr.rd, fp_binop(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), '/', instr.funct3, regs)); break; // FDIV.S
	case 0b0001101: regs.write_f(instr.rd, fp_binop(regs.read_f(instr.rs1), regs.read_f(instr.rs2), '/', instr.funct3, regs)); break; // FDIV.D
	case 0b0101100: write_f32_reg(regs, instr.rd, fp_sqrt(read_f32_reg(regs, instr.rs1), instr.funct3, regs)); break; // FSQRT.S
	case 0b0101101: regs.write_f(instr.rd, fp_sqrt(regs.read_f(instr.rs1), instr.funct3, regs)); break; // FSQRT.D

	case 0b0010000: write_f32_reg(regs, instr.rd, fsgnj_f32(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3)); break; // FSGNJ/N/X.S
	case 0b0010001: regs.write_f(instr.rd, fsgnj_f64(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3)); break; // FSGNJ/N/X.D
	case 0b0010100: write_f32_reg(regs, instr.rd, fminmax(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3 == 0b001, regs)); break; // FMIN/FMAX.S
	case 0b0010101: regs.write_f(instr.rd, fminmax(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3 == 0b001, regs)); break; // FMIN/FMAX.D

	case 0b1010000: regs.write_x(instr.rd, fcompare(read_f32_reg(regs, instr.rs1), read_f32_reg(regs, instr.rs2), instr.funct3, regs)); break; // FLE/FLT/FEQ.S
	case 0b1010001: regs.write_x(instr.rd, fcompare(regs.read_f(instr.rs1), regs.read_f(instr.rs2), instr.funct3, regs)); break; // FLE/FLT/FEQ.D

	case 0b1110000: // rs1 F -> rd integer: FMV.X.W (raw bits) / FCLASS.S
		if (instr.funct3 == 0b001) regs.write_x(instr.rd, fclassify(read_f32_reg(regs, instr.rs1)));
		else regs.write_x(instr.rd, sext32((uint32_t)bits_from_f64(regs.read_f(instr.rs1))));
		break;
	case 0b1110001: // rs1 D -> rd integer: FMV.X.D (RV64 only) / FCLASS.D
		if (instr.funct3 == 0b001) regs.write_x(instr.rd, fclassify(regs.read_f(instr.rs1)));
		else regs.write_x(instr.rd, bits_from_f64(regs.read_f(instr.rs1)));
		break;
	case 0b1111000: // FMV.W.X -- rd F, rs1 integer, raw bit move
		regs.write_f(instr.rd, f64_from_bits(box_f32((uint32_t)regs.read_x(instr.rs1))));
		break;
	case 0b1111001: // FMV.D.X (RV64 only)
		regs.write_f(instr.rd, f64_from_bits(regs.read_x(instr.rs1)));
		break;

	case 0b1100000: case 0b1100001: { // FCVT.W/WU/L/LU .S or .D -- rd integer; rs2 (reused) selects which
		double v = instr.fp_double ? regs.read_f(instr.rs1) : (double)read_f32_reg(regs, instr.rs1);
		std::feclearexcept(FE_ALL_EXCEPT);
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		uint64_t result = 0;
		switch (instr.rs2) {
		case 0b00000: result = sext32((uint32_t)fcvt_to_i32(v, regs)); break; // FCVT.W.*
		case 0b00001: result = sext32(fcvt_to_u32(v, regs)); break;          // FCVT.WU.* -- still sign-extended per spec
		case 0b00010: result = (uint64_t)fcvt_to_i64(v, regs); break;       // FCVT.L.*
		case 0b00011: result = fcvt_to_u64(v, regs); break;                 // FCVT.LU.*
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		regs.write_x(instr.rd, result);
		break;
	}

	case 0b1101000: { // FCVT.S.W/WU/L/LU -- rd F (single), rs1 integer. Converts straight to float
		// (not via a double intermediate) to avoid a double-rounding step.
		uint64_t xv = regs.read_x(instr.rs1);
		std::feclearexcept(FE_ALL_EXCEPT);
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		float fv = 0;
		switch (instr.rs2) {
		case 0b00000: fv = (float)(int32_t)xv; break;
		case 0b00001: fv = (float)(uint32_t)xv; break;
		case 0b00010: fv = (float)(int64_t)xv; break;
		case 0b00011: fv = (float)xv; break;
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		write_f32_reg(regs, instr.rd, fv);
		break;
	}

	case 0b1101001: { // FCVT.D.W/WU/L/LU -- rd D, rs1 integer
		uint64_t xv = regs.read_x(instr.rs1);
		std::feclearexcept(FE_ALL_EXCEPT);
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		double dv = 0;
		switch (instr.rs2) {
		case 0b00000: dv = (double)(int32_t)xv; break;
		case 0b00001: dv = (double)(uint32_t)xv; break;
		case 0b00010: dv = (double)(int64_t)xv; break;
		case 0b00011: dv = (double)xv; break;
		}
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		regs.write_f(instr.rd, dv);
		break;
	}

	case 0b0100000: { // FCVT.S.D -- narrow double to single (needs both F and D, enforced in decode())
		double a = regs.read_f(instr.rs1);
		std::feclearexcept(FE_ALL_EXCEPT);
		int old_round = std::fegetround();
		std::fesetround(host_round_mode(instr.funct3, regs.get_frm()));
		float result = (float)a;
		std::fesetround(old_round);
		regs.or_fflags(collect_fflags());
		write_f32_reg(regs, instr.rd, result);
		break;
	}
	case 0b0100001: { // FCVT.D.S -- widen single to double (exact -- always representable, no rounding needed)
		regs.write_f(instr.rd, (double)read_f32_reg(regs, instr.rs1));
		break;
	}

	default:
		break; // decoder gates unrecognized funct7/rs2/funct3 combinations to illegal before this is ever called
	}

	regs.set_pc(pc + instr.length);
}
