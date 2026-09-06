// Zawrs: wait-on-reservation-set.
//
// wrs.nto ("no timeout") and wrs.sto ("short timeout") stall the hart until
// the reservation set held by a preceding LR is invalidated, an interrupt
// arrives, or -- for the .sto form -- an implementation-defined short
// period passes. They exist so a spinlock's wait loop can idle instead of
// hammering the interconnect.
//
// Both are permitted to retire immediately. The spec is explicit that an
// implementation may treat them as a no-op, and the software contract is
// built around that: the surrounding loop always re-checks its condition
// after the instruction returns, because a wrs may return for any reason
// or none. So retiring at once is a correct implementation, not a stub --
// the same argument WFI already relies on in ext_zicsr.cpp.
//
// On a single-hart interpreter it is also the only implementation that
// terminates. There is no other hart that could invalidate the reservation,
// so a wrs.nto that genuinely waited for one would hang forever.
//
// The encodings sit in SYSTEM funct3=000 alongside ECALL/EBREAK/xRET/WFI,
// distinguished purely by their immediate (0x00D and 0x01D), which is why
// they are dispatched from ext_zicsr.cpp's fixed-immediate switch rather
// than being classified separately.
#include "riscv_decoder.hpp"
#include "riscv_core.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

void RiscvCore::exec_ZAWRS(const DecodedInstruction &instr, Registers &regs, Memory &mem)
{
	(void)mem;
	regs.set_pc(regs.get_pc() + instr.length);
}
