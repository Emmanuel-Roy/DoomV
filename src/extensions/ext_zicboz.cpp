// Zicboz: cache-block zeroing.
//
// cbo.zero writes zeros to the naturally-aligned cache block containing the
// address in rs1. Unlike its Zicbom siblings this is not a no-op anywhere --
// it is an architecturally visible store, and it is the reason Linux's
// clear_page is fast on hardware that has it.
//
// Two details are easy to get wrong and both matter:
//
//   * The address is *aligned down* to the block. rs1 names any byte in the
//     block, not its base, so `cbo.zero (a0)` with a0 = base+8 must still
//     zero from base. Missing the mask would write the wrong 64 bytes,
//     which on a page-clearing path corrupts the tail of the previous page.
//
//   * It is a store, so it takes store page faults and needs write
//     permission -- not read. Translating it as a load would let a
//     read-only page be silently zeroed.
//
// The block size is CBOZ_BLOCK_SIZE below, and is what tools/linux/dts/doomv.dts
// advertises as riscv,cboz-block-size. The two have to agree: Linux reads
// the DT value and issues exactly that many bytes' worth of cbo.zero per
// page, so a mismatch would leave part of each page uncleared.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "ext_cbo_gate.hpp"
#include "ext_h.hpp"
#include "mmu.hpp"
#include <cstdint>

namespace {
// 64 bytes: the value the device tree advertises, and the one Zic64b names
// as the profile-standard block size.
constexpr uint64_t CBOZ_BLOCK_SIZE = 64;
}

DecodedInstruction Decoder::decode_zicboz(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZICBOZ;
	instr.length = 4;
	instr.mnemonic = "CBO.ZERO";

	instr.opcode = raw_instr & 0x7F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.imm    = (raw_instr >> 20) & 0xFFF;
	return instr;
}

void RiscvCore::exec_ZICBOZ(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	// cbo.zero answers to CBZE in the same envcfg chain the other cache-
	// block instructions use, and for the same reason: this one actually
	// writes memory, so a hypervisor withholding it from a guest is
	// withholding a store it would rather emulate.
	switch (cbo::check(regs, cbo::CBZE)) {
	case cbo::DENY_ILLEGAL: raise_illegal_instruction(regs, instr.raw); return;
	case cbo::DENY_VIRTUAL:
		enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
		return;
	default: break;
	}

	uint64_t base = regs.read_x(instr.rs1) & ~(CBOZ_BLOCK_SIZE - 1);

	// Translate once per 8-byte chunk rather than once for the block: a
	// 64-byte block is well inside a 4KB page, but only because the base is
	// aligned -- the mask above is what guarantees the block cannot straddle
	// a page boundary. Translating each chunk keeps that guarantee from
	// being load-bearing.
	for (uint64_t off = 0; off < CBOZ_BLOCK_SIZE; off += 8) {
		uint64_t paddr;
		// A fault partway through leaves the earlier chunks zeroed. That is
		// permitted -- cbo.zero is not required to be atomic -- and the
		// trap reports the address that actually faulted.
		if (!translate_or_trap(regs, mem, base + off, AccessType::Store, paddr)) return;
		mem.write64(paddr, 0);
	}

	regs.set_pc(regs.get_pc() + instr.length);
}
