// A extension: atomic memory operations (LR/SC/AMO*). RiscvCore's only
// piece of cross-instruction state (the LR/SC reservation) is exclusively
// used here, so the constructor that initializes it lives in this file too.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "ext_zicfiss.hpp"
#include "ext_h.hpp"
#include "memory.hpp"
#include "extensions.hpp"

// Which A-space encodings are instructions on this hart, as Sail's
// amo_encoding_valid has it. Words and doublewords are A's (doublewords on
// RV64 only); bytes and halfwords are Zabha's, for the AMOs and -- with
// Zacas too -- amocas, never LR, SC or ssamoswap; amocas needs Zacas, and
// its quadword form (RV64) takes even register pairs, an odd one being
// illegal as Sail's configuration sets it.
static bool amo_encoding_valid(uint8_t amo_op, uint8_t funct3, uint8_t rd, uint8_t rs2)
{
	const bool lrsc = amo_op == 0b00010 || amo_op == 0b00011;
	const bool cas = amo_op == 0b00101;
	const bool ss = amo_op == 0b01001;
	const bool known = lrsc || cas || ss || amo_op == 0b00001 || amo_op == 0b00000 || amo_op == 0b00100
	                || amo_op == 0b01100 || amo_op == 0b01000 || amo_op == 0b10000 || amo_op == 0b10100
	                || amo_op == 0b11000 || amo_op == 0b11100;
	if (!known || (cas && !ExtSwitch.ZACAS)) return false;
	switch (funct3) {
	case 0b000: case 0b001: return ExtSwitch.ZABHA && !lrsc && !ss;
	case 0b010: return true;
	case 0b011: return Extensions.XLEN64;
	case 0b100: return cas && Extensions.XLEN64 && !(rd & 1) && !(rs2 & 1);
	default: return false;
	}
}

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

	// funct7[6:2] selects the op, funct3 the width -- 000 .B, 001 .H (Zabha),
	// 010 .W, 011 .D, 100 .Q (amocas.q only) -- and funct7[1:0] are aq/rl.
	const unsigned w = funct3 <= 4 ? funct3 : 0;
	instr.op_64 = (funct3 == 0b011);
	uint8_t amo_op = funct7 >> 2;
	switch (amo_op) {
	case 0b00010: { static const char *const n[5] = {"LR.B", "LR.H", "LR.W", "LR.D", "LR.Q"}; instr.mnemonic = n[w]; break; }
	case 0b00011: { static const char *const n[5] = {"SC.B", "SC.H", "SC.W", "SC.D", "SC.Q"}; instr.mnemonic = n[w]; break; }
	case 0b00001: { static const char *const n[5] = {"AMOSWAP.B", "AMOSWAP.H", "AMOSWAP.W", "AMOSWAP.D", "AMOSWAP.Q"}; instr.mnemonic = n[w]; break; }
	case 0b00000: { static const char *const n[5] = {"AMOADD.B", "AMOADD.H", "AMOADD.W", "AMOADD.D", "AMOADD.Q"}; instr.mnemonic = n[w]; break; }
	case 0b00100: { static const char *const n[5] = {"AMOXOR.B", "AMOXOR.H", "AMOXOR.W", "AMOXOR.D", "AMOXOR.Q"}; instr.mnemonic = n[w]; break; }
	case 0b01100: { static const char *const n[5] = {"AMOAND.B", "AMOAND.H", "AMOAND.W", "AMOAND.D", "AMOAND.Q"}; instr.mnemonic = n[w]; break; }
	case 0b01000: { static const char *const n[5] = {"AMOOR.B", "AMOOR.H", "AMOOR.W", "AMOOR.D", "AMOOR.Q"}; instr.mnemonic = n[w]; break; }
	case 0b10000: { static const char *const n[5] = {"AMOMIN.B", "AMOMIN.H", "AMOMIN.W", "AMOMIN.D", "AMOMIN.Q"}; instr.mnemonic = n[w]; break; }
	case 0b10100: { static const char *const n[5] = {"AMOMAX.B", "AMOMAX.H", "AMOMAX.W", "AMOMAX.D", "AMOMAX.Q"}; instr.mnemonic = n[w]; break; }
	case 0b11000: { static const char *const n[5] = {"AMOMINU.B", "AMOMINU.H", "AMOMINU.W", "AMOMINU.D", "AMOMINU.Q"}; instr.mnemonic = n[w]; break; }
	case 0b11100: { static const char *const n[5] = {"AMOMAXU.B", "AMOMAXU.H", "AMOMAXU.W", "AMOMAXU.D", "AMOMAXU.Q"}; instr.mnemonic = n[w]; break; }
	case 0b00101: { static const char *const n[5] = {"AMOCAS.B", "AMOCAS.H", "AMOCAS.W", "AMOCAS.D", "AMOCAS.Q"}; instr.mnemonic = n[w]; break; }
	case 0b01001: { static const char *const n[5] = {"SSAMOSWAP.B", "SSAMOSWAP.H", "SSAMOSWAP.W", "SSAMOSWAP.D", "SSAMOSWAP.Q"}; instr.mnemonic = n[w]; break; }
	}
	if (!amo_encoding_valid(amo_op, funct3, rd, rs2)) instr.ext = Extension::ILLEGAL;

	return instr;
}

RiscvCore::RiscvCore() : reservation_valid(false), reservation_addr(0)
{
}

void RiscvCore::exec_32A(const DecodedOp &instr, Registers &regs, Memory &mem)
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
	// ssamoswap answers to shadow-stack permissions, not ordinary AMO ones:
	// it may only touch a shadow stack page, which is what stops it being
	// used as a general-purpose atomic swap that happens to bypass the
	// write protection on one.
	AccessType amo_access = (amo_op == 0b00010) ? AccessType::Load
	                       : (amo_op == 0b00011) ? AccessType::Store
	                       : (amo_op == 0b01001) ? AccessType::ShadowStack
	                                              : AccessType::Amo;

	// With the extension disabled for this mode the encoding is not an
	// instruction at all. A guest refused by its hypervisor's envcfg gets a
	// virtual instruction so the hypervisor can emulate the swap; refused
	// by the machine's, an illegal one.
	if (amo_op == 0b01001 && !cfiss::enabled(regs)) {
		if (cfiss::denial_is_virtual(regs))
			enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
		else
			raise_illegal_instruction(regs, instr.raw);
		return;
	}
	// Every A-extension access must be naturally aligned, and unlike an
	// ordinary load or store this is not softened by Zicclsm: an atomic
	// spanning two naturally-aligned units is not something hardware can
	// perform. DoomV checked nothing here and quietly did the access.
	//
	// Two details here are not what they look like, and both were got
	// wrong before being read off the reference.
	//
	// The cause is an *access* fault, not an address-misaligned one. The A
	// extension explicitly permits either -- "an address-misaligned
	// exception or an access-fault exception will be generated" -- and the
	// access fault is the one to raise when the misaligned access is not
	// going to be emulated, which is this machine. Sail raises the access
	// fault, so cause 4/6 here is a mismatch even though it looks like the
	// more specific answer.
	//
	// And it comes *before* translation. An atomic to an address that is
	// both misaligned and unmapped reports the access fault, not the page
	// fault -- the misalignment is decided from the effective address
	// alone and never reaches the page tables.
	const unsigned width = 1u << instr.funct3;   // 1, 2, 4, 8 or 16 bytes (decode allows only legal ones)
	if (addr & (width - 1)) {
		constexpr uint64_t CAUSE_LOAD_ACCESS  = 5;
		constexpr uint64_t CAUSE_STORE_ACCESS = 7;
		enter_trap(regs, (amo_op == 0b00010) ? CAUSE_LOAD_ACCESS
		                                     : CAUSE_STORE_ACCESS, addr);
		return;
	}

	uint64_t paddr;
	if (!translate_or_trap(regs, mem, addr, amo_access, paddr, width)) return;

	// Zacas and Zabha: compare-and-swap at any width, and the byte and
	// halfword AMOs. The rest -- words and doublewords -- are below as ever.
	if (amo_op == 0b00101 || width < 4) {
		exec_amo_narrow_or_cas(instr, regs, mem, paddr, width);
		regs.set_pc(pc + instr.length);
		return;
	}

	if (amo_op == 0b00011) { // SC.W/SC.D
		if (reservation_valid && (reservation_addr & ~(RESERVATION_SET - 1)) == (paddr & ~(RESERVATION_SET - 1))) {
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
		reservation_addr = paddr;
		regs.write_x(instr.rd, loaded);
		regs.set_pc(pc + instr.length);
		return;
	}

	if (is64) {
		int64_t loaded_s = (int64_t)loaded, rs2_s = (int64_t)rs2_val;
		uint64_t result = loaded;
		switch (amo_op) {
		case 0b00001: result = rs2_val; break;                                          // AMOSWAP.D
		case 0b01001: result = rs2_val; break;                                          // SSAMOSWAP.D
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
		case 0b01001: result32 = rs2_32; break;                                               // SSAMOSWAP.W
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

// amocas at every width, and the AMOs at byte and halfword width (Zabha),
// as Sail's AMO clause does them. amocas compares the loaded value with rd's
// (a register pair for .Q, x0 reading as zero) and stores rs2 only if they
// are equal; every one leaves the loaded value, sign-extended, in rd.
void RiscvCore::exec_amo_narrow_or_cas(const DecodedOp &instr, Registers &regs, Memory &mem, uint64_t paddr,
                                       unsigned width)
{
	const uint8_t amo_op = instr.funct7 >> 2;
	const auto x = [&](unsigned r) -> uint64_t { return r ? regs.read_x((int)r) : 0; };
	if (width == 16) {
		const uint64_t lo = mem.read64(paddr), hi = mem.read64(paddr + 8);
		const uint64_t cmp_lo = x(instr.rd), cmp_hi = instr.rd ? x(instr.rd + 1u) : 0;
		if (lo == cmp_lo && hi == cmp_hi) {
			mem.write64(paddr, x(instr.rs2));
			mem.write64(paddr + 8, instr.rs2 ? x(instr.rs2 + 1u) : 0);
		}
		if (instr.rd) {
			regs.write_x(instr.rd, lo);
			regs.write_x(instr.rd + 1, hi);
		}
		return;
	}
	const unsigned bits = width * 8;
	const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
	const auto sext = [&](uint64_t v) -> uint64_t {
		return bits == 64 ? v : (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
	};
	uint64_t loaded = 0;
	switch (width) {
	case 1: loaded = mem.read8(paddr); break;
	case 2: loaded = mem.read16(paddr); break;
	case 4: loaded = mem.read32(paddr); break;
	default: loaded = mem.read64(paddr); break;
	}
	const uint64_t src = x(instr.rs2) & mask;
	const int64_t ls = (int64_t)sext(loaded), ss = (int64_t)sext(src);
	bool store = true;
	uint64_t result = src;
	switch (amo_op) {
	case 0b00101: store = loaded == (x(instr.rd) & mask); break;          // AMOCAS
	case 0b00001: result = src; break;                                    // AMOSWAP
	case 0b00000: result = (loaded + src) & mask; break;                  // AMOADD
	case 0b00100: result = loaded ^ src; break;                           // AMOXOR
	case 0b01100: result = loaded & src; break;                           // AMOAND
	case 0b01000: result = loaded | src; break;                           // AMOOR
	case 0b10000: result = ss < ls ? src : loaded; break;                 // AMOMIN
	case 0b10100: result = ss > ls ? src : loaded; break;                 // AMOMAX
	case 0b11000: result = src < loaded ? src : loaded; break;            // AMOMINU
	case 0b11100: result = src > loaded ? src : loaded; break;            // AMOMAXU
	default: break;
	}
	if (store) {
		switch (width) {
		case 1: mem.write8(paddr, (uint8_t)result); break;
		case 2: { const uint8_t b[2] = {(uint8_t)result, (uint8_t)(result >> 8)}; mem.write_bytes(paddr, b, 2); break; }
		case 4: mem.write32(paddr, (uint32_t)result); break;
		default: mem.write64(paddr, result); break;
		}
	}
	regs.write_x(instr.rd, sext(loaded));
}
