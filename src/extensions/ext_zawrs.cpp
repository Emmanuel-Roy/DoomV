// Zawrs: wait-on-reservation-set.
//
// wrs.nto ("no timeout") and wrs.sto ("short timeout") stall the hart until
// the reservation set held by a preceding LR is invalidated, an interrupt
// arrives, or -- for the .sto form -- an implementation-defined short
// period passes. They exist so a spinlock's wait loop can idle instead of
// hammering the interconnect.
//
// They wait as Sail's do with its configuration, where neither is a no-op.
// The wait ends at once if no reservation is held, when an interrupt is
// pending and enabled, or after the same ten clock ticks a WFI waits at
// most. A wrs.nto that times out below M-mode then traps -- illegal with
// mstatus.TW set, a virtual instruction with hstatus.VTW in a guest -- and
// otherwise both complete. DoomSystem::run_wait runs the wait. On a single
// hart nothing else can invalidate the reservation, so the timeout is what
// ends it.
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
	(void)regs;
	(void)mem;
	wait_request = ((instr.raw >> 20) & 0xFFF) == 0x01D ? Wait::WrsSto : Wait::WrsNto;
}
