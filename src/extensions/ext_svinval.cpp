// Svinval: fine-grained address-translation cache invalidation.
//
// Three instructions that split SFENCE.VMA into its parts, so a run of
// invalidations can be batched without paying for a full ordering fence
// between each one:
//
//   sfence.w.inval    order prior writes before the invalidations to follow
//   sinval.vma        invalidate one translation (rs1 = VA, rs2 = ASID)
//   sfence.inval.ir   order the invalidations before subsequent reads
//
// DoomV has no translation cache. mmu_translate walks the page table in
// guest memory on every single access, so a translation can never be stale
// and there is nothing to invalidate. All three retire without effect.
//
// That is a real implementation of the extension rather than a stub, and it
// is the same argument fence.i already relies on: a machine with no cache
// satisfies a cache-maintenance contract trivially. The distinction that
// matters is between "does nothing because nothing is required" and "does
// nothing because it was never implemented" -- the second is what a silent
// fall-through in the decoder produces, and this file exists so these
// encodings are the first kind.
//
// Encodings, from the assembler -- all SYSTEM, funct3=0, rd=0, next to
// SFENCE.VMA's funct7 of 0x09:
//
//   funct7 0x0B                      sinval.vma rs1, rs2
//   funct7 0x0C, rs1=0, rs2=0        sfence.w.inval
//   funct7 0x0C, rs1=0, rs2=1        sfence.inval.ir
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "extensions.hpp"
#include "memory.hpp"
#include <cstdint>

void RiscvCore::exec_SVINVAL(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;

	// All three are supervisor instructions. Executing one in U-mode is an
	// illegal instruction, and DoomV enforced nothing -- a user program
	// could invalidate translations. That the operations happen to be
	// no-ops here is beside the point: what a lower privilege level is
	// allowed to *attempt* is the observable part.
	if (regs.get_priv() == PrivMode::U) {
		// In VU-mode the exception is a *virtual* instruction. The rule is
		// general to the H extension: an instruction refused only because
		// the privilege is too low, but which HS-mode could have run,
		// belongs to the hypervisor to emulate or refuse -- so the guest's
		// own supervisor gets a chance to see it rather than a bare cause
		// 2 that says the instruction does not exist.
		if (Extensions.H && regs.get_virt()) {
			enter_trap(regs, 22, instr.raw);
			return;
		}
		raise_illegal_instruction(regs, instr.raw);
		return;
	}

	// SINVAL.VMA is governed by mstatus.TVM exactly as SFENCE.VMA is: it
	// invalidates translations, so a hypervisor watching a guest supervisor
	// manage its page tables has to see it. Trapping SFENCE.VMA and letting
	// SINVAL.VMA through leaves the guest a way to do the same job
	// unobserved -- which is what Svinval exists to provide, in bulk.
	//
	// SFENCE.W.INVAL and SFENCE.INVAL.IR (funct7 0b0001100) are *not*
	// affected: they only order the invalidations around them and name no
	// address, so there is nothing for TVM to observe.
	//
	// In VS-mode the governing bit is hstatus.VTVM instead, and the trap is
	// a virtual instruction rather than an illegal one, so the hypervisor
	// can emulate the invalidation rather than simply refusing it.
	if (instr.funct7 == 0b0001011 && Extensions.H && regs.get_virt()
	    && regs.get_priv() == PrivMode::S
	    && (regs.read_csr(0x600) & (1ull << 20))) { // hstatus.VTVM
		enter_trap(regs, 22, instr.raw);
		return;
	}

	if (instr.funct7 == 0b0001011 && regs.get_priv() == PrivMode::S
	    && (regs.read_csr(0x300) & (1ull << 20))) { // mstatus.TVM
		raise_illegal_instruction(regs, instr.raw);
		return;
	}

	// SINVAL.VMA invalidates translations, so it drops the TLB for the
	// same reason SFENCE.VMA does. The two ordering instructions
	// (SFENCE.W.INVAL, SFENCE.INVAL.IR) name no address and invalidate
	// nothing, so they are left alone -- flushing on them would be
	// harmless but would say something untrue about what they mean.
	if (instr.funct7 == 0b0001011) mmu_tlb_flush();
	regs.set_pc(regs.get_pc() + instr.length);
}
