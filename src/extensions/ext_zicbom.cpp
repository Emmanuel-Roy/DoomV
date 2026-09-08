// Zicbom: cache-block management.
//
// cbo.inval, cbo.clean and cbo.flush act on the cache block containing the
// address in rs1. DoomV has no cache -- every load and store goes straight
// to Memory -- so the coherence these instructions buy is already
// unconditionally true here, and all three retire without effect.
//
// That is a real implementation, not a stub: an emulator with no cache
// hierarchy satisfies the architectural contract trivially. What it does
// not model is the *permission* layer -- menvcfg.CBIE/CBCFE and
// senvcfg.CBIE let M-mode make these trap or degrade in S/U mode, and DoomV
// permits them at every privilege level. Nothing in the boot path
// restricts them, so the difference is unobservable today; it would matter
// to a guest deliberately testing the envcfg gating.
//
// They live in MISC-MEM (the FENCE opcode) at funct3=010, a slot base I
// leaves empty -- so unlike the hint extensions, disabling Zicbom really
// does make these illegal rather than reverting them to some base meaning.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "mmu.hpp"
#include <cstdint>
#include "ext_cbo_gate.hpp"
#include "ext_h.hpp"

DecodedInstruction Decoder::decode_zicbom(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZICBOM;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.imm    = (raw_instr >> 20) & 0xFFF; // selects which of the three

	switch (instr.imm) {
	case 0: instr.mnemonic = "CBO.INVAL"; break;
	case 1: instr.mnemonic = "CBO.CLEAN"; break;
	default: instr.mnemonic = "CBO.FLUSH"; break;
	}
	return instr;
}

// The block these operate on, matching Zicboz and what the device tree
// advertises. The address in rs1 may point anywhere inside the block; the
// block is the unit that gets checked.
constexpr uint64_t CBOM_BLOCK_SIZE = 64;

void RiscvCore::exec_ZICBOM(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	// Having no cache makes the *effect* of these instructions trivial, and
	// that is what the original implementation modelled: retire, do nothing.
	// It is not what makes them observable. A cache-block operation still
	// translates its address and still checks permissions, so an unmapped or
	// protected block faults exactly as a load or store would -- and that is
	// visible to software whether or not a cache exists behind it.
	//
	// All three take the same permission: read *or* write suffices, and a
	// read-only mapping may be cleaned, flushed and invalidated alike. The
	// obvious guess -- that CBO.INVAL demands write because it can discard
	// data -- is wrong, and the suite checks exactly that case.
	//
	// Nor is the D bit involved, since nothing is written. But a failure is
	// still reported as a *store* fault. AccessType::CacheBlock carries all
	// three of those rules; see its definition in mmu.hpp.
	// The permission layer, which used to be missing entirely: menvcfg,
	// henvcfg and senvcfg each carry a field that can make these trap, and
	// a hypervisor clears CBIE precisely so that a guest's cbo.inval comes
	// to it. cbo.inval answers to CBIE; clean and flush to CBCFE.
	//
	// A guest refused by the hypervisor's gate gets a virtual instruction,
	// not an illegal one -- see ext_cbo_gate.hpp for why the two differ.
	const cbo::Field field = (instr.imm == 0) ? cbo::CBIE : cbo::CBCFE;
	switch (cbo::check(regs, field)) {
	case cbo::DENY_ILLEGAL: raise_illegal_instruction(regs, instr.raw); return;
	case cbo::DENY_VIRTUAL:
		enter_trap(regs, hyp::CAUSE_VIRTUAL_INSTRUCTION, instr.raw);
		return;
	default: break;
	}

	uint64_t base = regs.read_x(instr.rs1) & ~(CBOM_BLOCK_SIZE - 1);
	uint64_t paddr;
	if (!translate_or_trap(regs, mem, base, AccessType::CacheBlock, paddr)) return;

	(void)mem;
	regs.set_pc(regs.get_pc() + instr.length);
}
