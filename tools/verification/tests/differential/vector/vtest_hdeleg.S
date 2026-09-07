# Differential test for guest trap delegation -- H's fourth increment.
#
# A trap taken in a guest can be delegated twice. medeleg sends it from M
# down to HS; hedeleg sends it one step further, to the guest's own handler.
# That second step is what makes virtualisation affordable: a guest kernel
# services its own breakpoints, page faults and system calls without the
# hypervisor being involved. A hypervisor that had to mediate every guest
# trap would be unusable.
#
# The properties checked here:
#
#   * Both levels must agree. A cause the hypervisor has not delegated stays
#     with the hypervisor even if the guest would like it -- the containment
#     guarantee, since otherwise a guest could claim traps its hypervisor
#     meant to inspect.
#
#   * A delegated trap goes to the guest's own trap registers and the hart
#     stays virtual, so from inside the guest it is indistinguishable from a
#     trap on real hardware. That is the entire point.
#
#   * The hypervisor's own sepc/scause must be untouched by a delegated
#     guest trap, or its state would be corrupted every time a guest took
#     one.
#
#   * Some causes cannot be delegated at all. hedeleg bits 10, 20, 21, 22
#     and 23 are read-only zero, each for the same reason: they name a trap
#     the hypervisor structurally cannot give away. An ecall from VS is how
#     a guest calls its hypervisor; a guest-page fault means the second
#     stage refused, which the guest cannot see let alone fix; a virtual
#     instruction is raised precisely because the guest attempted something
#     only the hypervisor may do.
#
# The ebreak pair is the delegation case proper: the same trap from the same
# instruction with only the hedeleg bit differing, so the destination is the
# only variable. The ecall pair is the read-only-zero case -- the bit is set
# and nothing changes. That pair was originally written expecting delegation
# to work, and spike is what said otherwise.
#
# Two constructions worth noting. The guest handler returns control by
# reading hstatus -- a virtual instruction (cause 22), deliberately never
# delegated -- rather than by ecall, because ecall is one of the causes
# under test and a delegated one would send the guest handler back to
# itself. And the traps used are ebreak and ecall rather than a page fault:
# an unmapped *physical* address would expose a known difference between
# these two implementations (DoomV has no physical-memory-attribute
# checking, so it does not fault where spike does), which has nothing to do
# with delegation.

.section .text.start
.global _start
_start:
	la	sp, __stacktop
	la	s0, begin_signature
	la	t0, m_handler
	csrw	mtvec, t0
	la	t0, hs_handler
	csrw	stvec, t0
	li	s2, 0			# HS-mode trap counter
	li	s3, 0			# most recent HS cause
	li	s5, 0			# guest trap counter
	li	s6, 0			# most recent guest cause

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- Delegate the causes under test from M down to HS. Without this
	#---- they stop at M-mode and hedeleg never gets a say.
	li	t0, (1 << 10) | (1 << 3) | (1 << 22)
	csrw	medeleg, t0

	#---- The guest's handler, plus markers in the guest's and the
	#---- hypervisor's trap registers so an untouched one is visible.
	la	t0, guest_handler
	csrw	vstvec, t0
	li	t0, 0x0BADBAD0
	csrw	vsepc, t0
	li	t0, 0x0FEED000
	csrw	sepc, t0
	li	t0, 0x0FEED001
	csrw	scause, t0

	#---- drop to HS-mode; a hypervisor runs there, and sret is what enters
	#---- a guest
	la	t0, hs_start
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11		# MPP = S
	csrs	mstatus, t0
	mret

hs_start:
#---------------------------------------------------------------------
# Stage 0: hedeleg = 0. The guest's ecall must stop at the HYPERVISOR.
	csrw	hedeleg, x0
	li	s4, 0
	la	t0, guest_ecall
	j	enter_guest

hs_after_undelegated:
	SIGX	s2			# HS trap count: 1
	SIGX	s3			# HS cause: 10 (ecall from VS)
	csrr	t1, vsepc
	SIGX	t1			# guest's vsepc untouched: 0x0BADBAD0
	SIGX	s5			# guest handler never ran: 0

#---------------------------------------------------------------------
# Stage 1: set hedeleg bit 10 and watch nothing change. That bit is
# read-only zero -- an ecall from VS-mode is how a guest calls its
# hypervisor, so delegating it back to the guest would leave the guest
# unable to call out at all. This stage was written expecting delegation to
# work; spike disagreed, and spike was right.
	li	t0, 1 << 10
	csrw	hedeleg, t0
	li	s2, 0
	li	s3, 0
	li	s5, 0
	li	s6, 0
	li	s4, 1
	la	t0, guest_ecall
	j	enter_guest

hs_after_delegated:
	SIGX	s5			# guest handler still did NOT run: 0
	SIGX	s6			# so no guest cause was recorded: 0
	SIGX	s2			# the hypervisor got it anyway: 1
	csrr	t1, sepc
	SIGX	t1			# hypervisor's sepc untouched
	csrr	t1, scause
	SIGX	t1			# and its scause

#---------------------------------------------------------------------
# Stage 2: a breakpoint, delegated. The guest handles its own ebreak.
	li	t0, 1 << 3
	csrw	hedeleg, t0
	li	s2, 0
	li	s5, 0
	li	s6, 0
	li	s4, 2
	la	t0, guest_ebreak
	j	enter_guest

hs_after_ebreak_delegated:
	SIGX	s5			# guest handled it: 1
	SIGX	s6			# cause 3: breakpoint
	SIGX	s2			# hypervisor saw only the handover: 0
	csrr	t1, vsepc
	SIGX	t1			# the guest's vsepc now points at its ebreak

#---------------------------------------------------------------------
# Stage 3: the same breakpoint with hedeleg cleared. Same instruction, same
# address; only the delegation bit differs.
	csrw	hedeleg, x0
	li	s2, 0
	li	s3, 0
	li	s5, 0
	li	s4, 3
	la	t0, guest_ebreak
	j	enter_guest

hs_after_ebreak_undelegated:
	SIGX	s2			# hypervisor got it: 1
	SIGX	s3			# cause 3
	SIGX	s5			# guest handler never ran: 0

	j	sig_done

#---------------------------------------------------------------------
# Enter the guest at the address in t0. satp/vsatp/hgatp all stay bare, so
# this suite is about delegation and nothing else.
enter_guest:
	csrw	sepc, t0
	li	t0, 1 << 7		# hstatus.SPV -> sret enters VS
	csrw	hstatus, t0
	li	t0, 1 << 8		# sstatus.SPP = S: land in VS, not VU
	csrs	sstatus, t0
	sret

guest_ecall:
	ecall
	j	guest_ecall		# not reached

guest_ebreak:
	ebreak
	j	guest_ebreak		# not reached

#---------------------------------------------------------------------
# The guest's own handler, reached only when hedeleg delegated the cause.
# It hands control back by reading hstatus: a virtual instruction, cause 22,
# which is deliberately never delegated. Using ecall here would send a
# delegated ecall straight back to this handler.
	.align 4
guest_handler:
	addi	s5, s5, 1
	csrr	s6, vscause
	csrr	t0, hstatus		# cause 22 -> the hypervisor
	j	guest_handler		# not reached

#---------------------------------------------------------------------
# HS-mode handler. Cause 22 means the guest handler is done; anything else
# is the trap under test arriving because it was not delegated.
	.align 4
hs_handler:
	csrr	t6, scause
	li	t5, 22
	beq	t6, t5, 1f
	addi	s2, s2, 1
	mv	s3, t6
1:
	li	t5, 1 << 7
	csrc	hstatus, t5		# SPV = 0: the sret below stays in HS
	li	t5, 0
	beq	s4, t5, 3f
	li	t5, 1
	beq	s4, t5, 4f
	li	t5, 2
	beq	s4, t5, 5f
	la	t5, hs_after_ebreak_undelegated
	j	9f
3:	la	t5, hs_after_undelegated
	j	9f
4:	la	t5, hs_after_delegated
	j	9f
5:	la	t5, hs_after_ebreak_delegated
9:
	csrw	sepc, t5
	li	t5, 1 << 8
	csrs	sstatus, t5		# SPP = S: sret returns to HS
	sret

	.align 4
m_handler:
	# Nothing should reach M-mode here. If something does, make it visible
	# in the signature rather than hanging.
	addi	s2, s2, 100
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

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
