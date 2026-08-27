# Hand-written verification for Stage 2's timer (CLINT mtime/mtimecmp +
# the check_and_take_interrupt path in DoomSystem::step()). Same
# breakpoint/crash.log methodology as the Stage 1 tests.
#
# M-mode arms mtimecmp a short, deliberately-imprecise distance ahead
# (mtime ticks once per retired instruction -- see Timer::tick, called
# from Memory::step_instructions), enables MTIE+mstatus.MIE, then spins
# forever. The interrupt should fire mid-spin regardless of exactly how
# many setup instructions ran first. On the first trap: check mcause,
# disarm mtimecmp to the far future, redirect mepc to a second bounded
# spin, and MRET. If a second trap fires during that second spin (it
# shouldn't -- disarming should have stopped it), the handler marks
# failure instead of the success value.
.section .text.start
.global _start

.equ CLINT_MTIMECMP, 0x02004000
.equ CLINT_MTIME,    0x0200BFF8
.equ MIE_MTIE,       (1 << 7)
.equ MSTATUS_MIE,    (1 << 3)

_start:
	la   t0, m_trap
	csrw mtvec, t0

	li   t0, CLINT_MTIME
	ld   t1, 0(t0)
	addi t1, t1, 30
	li   t0, CLINT_MTIMECMP
	sd   t1, 0(t0)

	li   t0, MIE_MTIE
	csrw mie, t0
	li   t0, MSTATUS_MIE
	csrs mstatus, t0

	li   s0, 0
spin1:
	addi s0, s0, 1
	j    spin1

	.align 2
m_trap:
	csrr a0, mcause        # expect (1<<63)|7 on the first (and only expected) trap
	bnez a1, second_trap   # a1 starts at 0 (reset state) -- nonzero means we've been here before

	li   a1, 1
	li   t0, CLINT_MTIMECMP
	li   t1, -1            # disarm: push mtimecmp to the far future
	sd   t1, 0(t0)
	la   t0, spin2
	csrw mepc, t0          # redirect the upcoming MRET into the second, bounded spin
	mret

second_trap:
	li   a0, 0xbaadf00d    # disarming failed to prevent an immediate re-fire
	j    halt

	.align 2
spin2:
	addi s1, s1, 1
	li   t2, 50
	blt  s1, t2, spin2
	li   a0, 0x600dc0de    # success: completed the second spin with no re-fire
	j    halt

halt:
	j    halt
