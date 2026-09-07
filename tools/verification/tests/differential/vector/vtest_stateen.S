# Differential test for Ssstateen and Sscofpmf -- the two RVA23 mandatory
# extensions that a hand-written phase plan missed.
#
# Both are CSRs and an interrupt with no instructions of their own, which is
# exactly why they went unnoticed until DoomV was audited against the
# profile's own machine-readable extension list rather than against my
# reading of it.
#
# Ssstateen gates access to state that newer extensions add. Its bits are
# mostly read-only zero here because the state they would gate does not
# exist on this machine, and reporting that honestly is the point: software
# reads these registers to discover what it must save across a context
# switch, so a bit that reads back set after a write claims state the
# machine does not have.
#
# The gating is hierarchical, and that hierarchy is the interesting part.
# mstateen0.SE0 controls whether sstateen0 is reachable from below M at all.
# Denying at the top denies all the way down -- which is what lets a
# hypervisor withhold state it does not understand well enough to
# context-switch, turning a silent cross-guest corruption into a clean trap.
#
# Sscofpmf adds an overflow bit per counter and a local counter-overflow
# interrupt, so a profiler can sample rather than poll. This hart counts no
# events, so nothing overflows on its own -- but the registers, their WARL
# behaviour and scountovf's derived read are all observable, and software
# can drive an overflow by setting the bit directly.

.section .text.start
.global _start
_start:
	la	sp, __stacktop
	la	s0, begin_signature
	la	t0, trap_handler
	csrw	mtvec, t0
	li	s2, 0			# trap counter
	li	s3, 0			# most recent cause

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- Ssstateen: writable bits read back, the rest read zero. Writing
	#---- all-ones and reading is how software discovers which state this
	#---- machine actually has.
	li	t0, -1
	csrw	mstateen0, t0
	csrr	t1, mstateen0
	SIGX	t1
	csrw	mstateen0, x0
	csrr	t1, mstateen0
	SIGX	t1			# zero after clearing

	#---- the other three indices behave the same way
	li	t0, -1
	csrw	mstateen1, t0
	csrr	t1, mstateen1
	SIGX	t1
	csrw	mstateen2, t0
	csrr	t1, mstateen2
	SIGX	t1

	#---- sstateen and hstateen are separate storage, not aliases
	li	t0, -1
	csrw	sstateen0, t0
	csrr	t1, sstateen0
	SIGX	t1
	li	t0, -1
	csrw	hstateen0, t0
	csrr	t1, hstateen0
	SIGX	t1
	csrr	t1, mstateen0
	SIGX	t1			# mstateen0 still zero: they are distinct

	#---- Sscofpmf: mhpmevent's OF bit and mode-inhibit bits are writable
	li	t0, 1
	slli	t0, t0, 63		# OF
	csrw	mhpmevent3, t0
	csrr	t1, mhpmevent3
	SIGX	t1

	#---- scountovf reflects it, derived rather than stored
	csrr	t1, scountovf
	SIGX	t1			# bit 3 set

	#---- a second counter, to show the packing is by index
	li	t0, 1
	slli	t0, t0, 63
	csrw	mhpmevent5, t0
	csrr	t1, scountovf
	SIGX	t1			# bits 3 and 5

	#---- LCOFI (interrupt 13) must now be pending in mip
	csrr	t1, mip
	srli	t1, t1, 13
	andi	t1, t1, 1
	SIGX	t1			# 1

	#---- clearing OF clears both the scountovf bit and the interrupt,
	#---- because both are derived from the same state rather than latched
	csrw	mhpmevent3, x0
	csrw	mhpmevent5, x0
	csrr	t1, scountovf
	SIGX	t1			# 0
	csrr	t1, mip
	srli	t1, t1, 13
	andi	t1, t1, 1
	SIGX	t1			# 0

	#---- the mode-inhibit bits are writable too
	li	t0, 0x1F
	slli	t0, t0, 58		# OF | MINH | SINH | UINH | VSINH
	csrw	mhpmevent4, t0
	csrr	t1, mhpmevent4
	SIGX	t1

	#---- scountovf is read-only: writing it is an illegal instruction
	li	s2, 0
	li	s3, 0
	li	t0, -1
	csrw	scountovf, t0
	SIGX	s2			# 1
	SIGX	s3			# 2 (illegal instruction)

	#---- The stateen hierarchy. With mstateen0.SE0 clear, S-mode may not
	#---- reach sstateen0 at all -- denying at the top denies below.
	csrw	mstateen0, x0
	li	s4, 0
	la	t0, s_denied
	j	enter_smode

s_denied:
	li	s2, 0
	li	s3, 0
	csrr	t1, sstateen0
	SIGX	s2			# 1: denied
	SIGX	s3			# 2: illegal instruction
	ecall

m_allow:
	#---- now set mstateen0.SE0 and the identical access must succeed
	li	t0, 1
	slli	t0, t0, 63
	csrw	mstateen0, t0
	li	s4, 1
	la	t0, s_allowed
	j	enter_smode

s_allowed:
	li	s2, 0
	csrr	t1, sstateen0
	SIGX	s2			# 0: permitted now
	ecall

m_done:
.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

enter_smode:
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11		# MPP = S
	csrs	mstatus, t0
	mret

	.align 4
trap_handler:
	csrr	t6, mcause
	li	t5, 9			# ecall from S: advance a stage
	beq	t6, t5, next_stage
	mv	s3, t6
	addi	s2, s2, 1
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

next_stage:
	li	t5, 3 << 11
	csrs	mstatus, t5		# back to M-mode
	beqz	s4, 1f
	la	t5, m_done
	j	9f
1:	la	t5, m_allow
9:
	csrw	mepc, t5
	mret

	.section .data
	.align 4
	.global begin_signature
begin_signature:
	.fill	512, 1, 0
	.global end_signature
end_signature:

	.section .tohost, "aw", @progbits
	.align 8
	.global tohost
tohost:
	.dword	0
	.global fromhost
fromhost:
	.dword	0
