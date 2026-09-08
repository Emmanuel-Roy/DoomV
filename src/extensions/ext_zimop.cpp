// Zimop: may-be-operations.
//
// Thirty-two MOP.R.n and eight MOP.RR.n encodings that are architecturally
// defined to write zero to rd and do nothing else. The point is forward
// compatibility: they are reserved encodings with *defined* behaviour, so a
// future extension can claim one and older software still runs -- it sees a
// zero rather than an illegal instruction.
//
// That makes "write zero" the whole specification, and getting it wrong in
// the obvious way (leaving rd untouched) would be invisible until some
// guest depended on the zero.
//
// They live in SYSTEM funct3=100, a slot Zicsr leaves unused. Bit 31 marks
// the space and bit 25 picks the form: clear for MOP.R (one source), set
// for MOP.RR (two). The n index is scattered across the remaining bits and
// carries no meaning here, since every n behaves identically.
#include "riscv_decoder.hpp"
#include "ext_zicfiss.hpp"
#include "mmu.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

DecodedInstruction Decoder::decode_zimop(uint32_t raw_instr) const
{
	DecodedInstruction instr{};
	instr.ext = Extension::ZIMOP;
	instr.length = 4;

	instr.opcode = raw_instr & 0x7F;
	instr.rd     = (raw_instr >> 7) & 0x1F;
	instr.funct3 = (raw_instr >> 12) & 0x07;
	instr.rs1    = (raw_instr >> 15) & 0x1F;
	instr.rs2    = (raw_instr >> 20) & 0x1F;
	instr.funct7 = (raw_instr >> 25) & 0x7F;

	instr.mnemonic = ((raw_instr >> 25) & 1) ? "MOP.RR" : "MOP.R";
	return instr;
}

void RiscvCore::exec_ZIMOP(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	// Zicfiss claims three of these encodings, but only while it is
	// enabled for the current mode. When it is not -- because the hart
	// lacks it, or because the envcfg chain says no -- they fall through
	// to the may-be-operation below and write zero. That is not a fallback
	// but the specified behaviour, and it is what lets one binary built
	// with shadow stacks run on a hart without them.
	if (cfiss::enabled(regs)) {
		const uint64_t xlen_bytes = 8;
		if (cfiss::is_sspush(instr)) {
			// Push rs2 to the shadow stack. ssp grows down, and the store
			// goes through AccessType::ShadowStack so it lands only on a
			// page marked as one.
			uint64_t ssp = regs.read_csr(cfiss::CSR_SSP) - xlen_bytes;
			uint64_t paddr;
			if (!translate_or_trap(regs, mem, ssp, AccessType::ShadowStack,
			                       paddr, xlen_bytes))
				return;
			mem.write64(paddr, regs.read_x(instr.rs2));
			regs.write_csr(cfiss::CSR_SSP, ssp);
			regs.set_pc(regs.get_pc() + instr.length);
			return;
		}
		if (cfiss::is_sspopchk(instr)) {
			// Pop and compare. A mismatch means the ordinary stack's copy
			// of the return address was changed after it was pushed, which
			// is the attack this extension exists to catch, so it raises a
			// software check rather than returning anywhere.
			uint64_t ssp = regs.read_csr(cfiss::CSR_SSP);
			uint64_t paddr;
			if (!translate_or_trap(regs, mem, ssp, AccessType::ShadowStack,
			                       paddr, xlen_bytes))
				return;
			if (mem.read64(paddr) != regs.read_x(instr.rs1)) {
				// tval 3 names the shadow-stack check, as 2 names the
				// landing-pad one.
				raise_software_check(regs, 3);
				return;
			}
			regs.write_csr(cfiss::CSR_SSP, ssp + xlen_bytes);
			regs.set_pc(regs.get_pc() + instr.length);
			return;
		}
		if (cfiss::is_ssrdp(instr)) {
			regs.write_x(instr.rd, regs.read_csr(cfiss::CSR_SSP));
			regs.set_pc(regs.get_pc() + instr.length);
			return;
		}
	}

	(void)mem;
	regs.write_x(instr.rd, 0);
	regs.set_pc(regs.get_pc() + instr.length);
}
