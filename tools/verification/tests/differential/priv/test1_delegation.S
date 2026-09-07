# Hand-written verification for Stage 1's privilege/delegation plumbing --
# there's no formal riscv-arch-test suite for this (see README notes in
# run_arch_test.py), so this is checked by hand: run under DoomV with a
# breakpoint at `halt`, then read the resulting crash.log for a0/a1/priv/
# scause instead of diffing a signature against spike.
#
# Sequence: M-mode delegates ECALL-from-S (medeleg bit 9) to S-mode, sets
# mstatus.MPP=S, and MRETs into S-mode code that immediately ECALLs.
# Expected outcome: the trap lands in s_trap (stvec), not m_trap (mtvec) --
# proving delegation actually routes the trap to S instead of M -- with
# scause==9 and priv still S (SRET back to S never even needs to run for
# this to be a valid test, since S-mode never left).
.section .text.start
.global _start

_start:
	la   t0, m_trap
	csrw mtvec, t0
	la   t0, s_trap
	csrw stvec, t0

	li   t0, (1 << 9)      # medeleg[9] = ECALL-from-S
	csrw medeleg, t0

	# mstatus.MPP (bits 12:11) = 0b01 (S)
	csrr t1, mstatus
	li   t2, ~0x1800
	and  t1, t1, t2
	li   t2, (1 << 11)
	or   t1, t1, t2
	csrw mstatus, t1

	la   t0, s_entry
	csrw mepc, t0
	mret

s_entry:
	# Now running in S-mode (priv should read back as 1).
	li   a0, 0xdead0000
	ecall
	li   a0, 0xbaadf00d     # must NOT execute -- would mean the ecall fell through untrapped
	j    halt

	# mtvec/stvec are spec-required to be 4-byte aligned (the low 2 bits are
	# a mode field, not part of the address) -- enter_trap masks them off
	# accordingly, so an unaligned handler here would silently get
	# truncated to 2 bytes short of where it actually is.
	.align 2
s_trap:
	csrr a1, scause         # expect 9 (ECALL from S)
	li   a0, 0x600dc0de     # marker: delegation worked, landed in S's own handler
	j    halt

	.align 2
m_trap:
	# Reaching here means delegation failed and M's handler caught it instead.
	li   a0, 0xf00dbaad
	j    halt

halt:
	j    halt
