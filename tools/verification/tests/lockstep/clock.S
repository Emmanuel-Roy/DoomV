# Sail's clock and counters, lock-stepped. Nothing here checks a value
# itself: every register and CSR write is compared against Sail's trace,
# so the test only has to make the counters, the clock and the waits do
# something observable. See tools/verification/lockstep_sail.py.
#
#   mcycle, minstret, time, cycle, instret, mtime -- read, written, read back
#   mcountinhibit and the Smcntrpmf mode filters
#   mhpmcounter3, mhpmevent3, hpmcounter3, and the absent 0xB01
#   the counter gates from S and U
#   WFI woken by the timer, WFI timing out in M, in S with TW, and from U
#   WRS with and without a reservation
#
# Traps go to M. An ecall from S or U returns to M at s11; any other
# exception skips the instruction; the timer interrupt is disarmed.

.equ CLINT_MTIMECMP, 0x02004000
.equ CLINT_MTIME,    0x0200BFF8
.equ MSTATUS_MPP,    (3 << 11)
.equ MSTATUS_TW,     (1 << 21)

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, -1
	csrw pmpaddr0, t0
	li   t0, 0x1f
	csrw pmpcfg0, t0
	csrw medeleg, zero
	csrw mideleg, zero

	# ---- counters in M -------------------------------------------------------
	csrr a0, mcycle
	csrr a1, minstret
	csrr a2, time
	csrr a3, cycle
	csrr a4, instret
	li   t0, CLINT_MTIME
	ld   a5, 0(t0)
	li   t0, 100
	csrw mcycle, t0
	csrr a0, mcycle
	li   t0, 200
	csrw minstret, t0
	csrr a1, minstret
	csrr a1, minstret
	csrrw a2, minstret, t0
	csrr a3, minstret

	li   t0, -1
	csrw mcountinhibit, t0
	csrr a0, mcountinhibit
	csrr a1, mcycle
	csrr a2, minstret
	csrr a1, mcycle
	csrr a2, minstret
	csrw mcountinhibit, zero

	li   t0, -1
	csrw mcyclecfg, t0
	csrr a0, mcyclecfg
	csrw minstretcfg, t0
	csrr a1, minstretcfg
	csrr a2, mcycle
	csrr a3, minstret
	csrr a2, mcycle
	csrr a3, minstret
	li   t0, (1 << 61) | (1 << 60)   # SINH, UINH: M still counts
	csrw mcyclecfg, t0
	csrw minstretcfg, t0
	csrr a2, mcycle
	csrr a3, minstret

	li   t0, -1
	csrw mhpmcounter3, t0
	csrr a0, mhpmcounter3
	csrw mhpmevent3, t0
	csrr a1, mhpmevent3
	csrr a2, hpmcounter3
	csrr a3, 0xB01                   # absent: illegal
	csrw mhpmcounter3, zero

	# ---- WFI in M: a timer wakes it -----------------------------------------
	li   t0, CLINT_MTIME
	ld   t1, 0(t0)
	addi t1, t1, 3
	li   t0, CLINT_MTIMECMP
	sd   t1, 0(t0)
	li   t0, (1 << 7)
	csrw mie, t0
	wfi                              # wakes after three ticks; MIE is clear
	csrr a0, mip
	csrr a1, time
	li   t0, (1 << 3)
	csrs mstatus, t0                 # now the interrupt is taken
	nop
	csrc mstatus, t0

	# ---- WFI in M with nothing pending: times out, completes ----------------
	csrw mie, zero
	csrr a0, time
	wfi
	csrr a1, time
	csrr a2, mcycle
	csrr a3, minstret

	# ---- WRS in M ----------------------------------------------------------------
	.word 0x00D00073                 # wrs.nto, no reservation: ends at once
	csrr a0, time
	la   t1, scratch
	lr.d t2, 0(t1)
	.word 0x01D00073                 # wrs.sto with a reservation: times out
	csrr a0, time
	lr.d t2, 0(t1)
	.word 0x00D00073                 # wrs.nto in M: times out, completes
	csrr a0, time
	sc.d t3, t2, 0(t1)

	# ---- S-mode -------------------------------------------------------------------
	li   t0, 7
	csrw mcounteren, t0
	csrw scounteren, t0
	la   s11, back_from_s
	li   t0, MSTATUS_MPP
	csrc mstatus, t0
	li   t0, (1 << 11)
	csrs mstatus, t0
	la   t0, s_mode
	csrw mepc, t0
	mret
s_mode:
	csrr a0, cycle
	csrr a1, time
	csrr a2, instret
	csrr a3, hpmcounter3             # mcounteren bit 3 clear: illegal
	wfi                              # TW clear: times out, completes
	csrr a0, time
	ecall
back_from_s:
	csrw mcounteren, zero
	li   t0, MSTATUS_TW
	csrs mstatus, t0
	li   t0, (1 << 11)
	csrs mstatus, t0
	la   s11, back_from_s2
	la   t0, s_mode2
	csrw mepc, t0
	mret
s_mode2:
	csrr a0, cycle                   # mcounteren clear: illegal
	wfi                              # TW set: times out, then illegal
	csrr a0, time
	la   t1, scratch
	lr.d t2, 0(t1)
	.word 0x00D00073                 # wrs.nto below M with TW: illegal on timeout
	ecall
back_from_s2:
	li   t0, MSTATUS_TW
	csrc mstatus, t0

	# ---- Filters and U-mode ------------------------------------------------------
	li   t0, 7
	csrw mcounteren, t0
	li   t0, (1 << 60)               # UINH: U does not count
	csrw mcyclecfg, t0
	csrw minstretcfg, t0
	li   t0, MSTATUS_MPP
	csrc mstatus, t0
	la   s11, back_from_u
	la   t0, u_mode
	csrw mepc, t0
	mret
u_mode:
	csrr a0, cycle
	csrr a1, instret
	nop
	nop
	csrr a0, cycle
	csrr a1, instret
	wfi                              # not available to U: illegal at once
	ecall
back_from_u:
	csrw mcyclecfg, zero
	csrw minstretcfg, zero
	csrr a0, mcycle
	csrr a1, minstret

	# ---- done ----------------------------------------------------------------------
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	bltz t5, interrupt
	li   t6, 8
	beq  t5, t6, from_lower
	li   t6, 9
	beq  t5, t6, from_lower
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret
from_lower:
	li   t6, MSTATUS_MPP
	csrs mstatus, t6
	csrw mepc, s11
	mret
interrupt:
	li   t6, CLINT_MTIMECMP
	li   t5, -1
	sd   t5, 0(t6)
	mret

.section .data
.align 3
scratch: .dword 0
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
