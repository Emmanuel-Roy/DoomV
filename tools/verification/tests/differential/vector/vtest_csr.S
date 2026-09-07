# Differential test for phase 2: Zicboz, Zawrs, the Zicntr counters, and the
# mcounteren/scounteren gating that decides who may read them.
#
# The counters need a different approach from every other test here. Their
# *values* are not comparable: spike counts host-simulated cycles, DoomV
# counts retired instructions, and neither is wrong. So this test never dumps
# a counter value. It dumps the properties both machines must agree on --
# that a counter never goes backwards, and that reading one from a mode
# mcounteren has not enabled raises an illegal-instruction trap.
#
# That second property is the interesting one, because until this change
# DoomV enforced no CSR privilege at all: every mode could read and write
# every CSR. A test that only read counters from M-mode would pass
# identically before and after.
#
# Runs in M-mode with satp=0, dropping to S-mode without page tables (bare
# translation) where the gating actually applies. Traps go to M via mtvec.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	# Permit-all PMP entry, installed before any drop to S-mode.
	#
	# With PMP implemented and no entry configured, the spec denies S and U
	# mode access outright -- so a test that drops privilege takes a fetch
	# access fault before its first instruction there. spike happens to
	# default permissively and these tests silently relied on that for a
	# long time; Sail implements it faithfully and refused, which is how the
	# dependency was found. Setting it up here is what real software does and
	# makes the suite independent of any reference's defaults.
	#
	# pmpaddr0 all-ones with A=NAPOT covers the whole address space; 0x1F is
	# NAPOT plus R, W and X.
	li	t0, -1
	csrw	pmpaddr0, t0
	li	t0, 0x1F
	csrw	pmpcfg0, t0
	la	s0, begin_signature
	la	t0, trap_handler
	csrw	mtvec, t0
	li	s2, 0			# trap counter
	li	s3, 0			# mcause of the first trap seen

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- Zicntr: the counters must not run backwards. Reading twice and
	#---- comparing is the only claim about their values that is portable
	#---- across two machines with different notions of a cycle.
	csrr	t0, cycle
	csrr	t1, cycle
	sltu	t2, t1, t0		# 1 if the second read was smaller
	xori	t2, t2, 1		# -> 1 means monotonic
	SIGX	t2
	csrr	t0, time
	csrr	t1, time
	sltu	t2, t1, t0
	xori	t2, t2, 1
	SIGX	t2
	csrr	t0, instret
	csrr	t1, instret
	sltu	t2, t1, t0
	xori	t2, t2, 1
	SIGX	t2

	#---- Zihpm: hpmcounter3..31 must be readable, not illegal. They are
	#---- permitted to read as zero, and do here -- this machine counts no
	#---- events, and inventing a number would be worse than reporting none.
	csrr	t0, hpmcounter3
	SIGX	t0
	csrr	t0, hpmcounter31
	SIGX	t0

	#---- Zicboz. The block is 64 bytes and the address is aligned *down*,
	#---- so passing base+8 must still zero from base. Guard words on
	#---- either side of the block catch a zeroing that ran wide.
	la	t3, cbz_area		# 64-byte aligned; guard, block, guard
	li	t4, -1
	sd	t4, 0(t3)		# guard before
	sd	t4, 64(t3)		# block word 0   (block starts at +64)
	sd	t4, 88(t3)		# block word 3
	sd	t4, 120(t3)		# block word 7
	sd	t4, 128(t3)		# guard after
	addi	t5, t3, 64+8		# a byte *inside* the block, not its base
	cbo.zero (t5)
	ld	t0, 64(t3)
	SIGX	t0			# must be 0 -- aligned down to the base
	ld	t0, 88(t3)
	SIGX	t0			# must be 0
	ld	t0, 120(t3)
	SIGX	t0			# must be 0 -- last word of the block
	ld	t0, 0(t3)
	SIGX	t0			# guard before: must still be -1
	ld	t0, 128(t3)
	SIGX	t0			# guard after: must still be -1

	#---- Zawrs. Both forms must retire and disturb nothing. On one hart a
	#---- wrs that genuinely waited for another hart to break a reservation
	#---- would never return, so "retires immediately" is also the only
	#---- terminating implementation.
	li	a0, 0x1234567812345678
	li	a1, 0x8765432187654321
	wrs.nto
	wrs.sto
	SIGX	a0
	SIGX	a1

	#---- Counter gating. mcounteren=0 means S-mode may not read any of
	#---- them; each attempt must raise an illegal-instruction trap.
	csrw	mcounteren, x0
	la	t0, smode_denied
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11		# MPP = S
	csrs	mstatus, t0
	mret

smode_denied:
	csrr	t0, cycle		# traps; handler skips it
	csrr	t0, time		# traps
	ecall				# back to M, at m_after_denied

m_after_denied:
	SIGX	s2			# traps taken: must be exactly 2
	SIGX	s3			# mcause of the first: must be 2 (illegal)

	#---- Now permit CY/TM/IR in S-mode and confirm the same reads work.
	li	t0, 7
	csrw	mcounteren, t0
	li	s2, 0
	la	t0, smode_allowed
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

smode_allowed:
	csrr	t0, cycle		# must NOT trap now
	csrr	t0, time
	csrr	t0, instret
	ecall

m_after_allowed:
	SIGX	s2			# traps taken: must be 0

	#---- A read-only CSR must refuse a write even from M-mode. cycle is
	#---- read-only by its address (csr[11:10] == 11), so this is a
	#---- privilege-independent rule, unlike the gating above.
	li	s2, 0
	li	t1, 1
	csrw	cycle, t1		# illegal: writing a read-only CSR
	SIGX	s2			# must be 1

.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

#---------------------------------------------------------------------
# Trap handler, M-mode. Two cases:
#   ecall from S (mcause 9) -- the test's way of getting back to M-mode;
#                              resume at the continuation this stage wants.
#   anything else           -- count it, remember the first cause, and skip
#                              the offending 4-byte instruction.
	.align 4
trap_handler:
	csrr	t6, mcause
	li	t5, 9
	beq	t6, t5, from_smode

	addi	s2, s2, 1
	bnez	s3, 1f
	mv	s3, t6			# first cause only
1:
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

from_smode:
	# Return to M-mode at whichever continuation matches the stage we are
	# in. s4 holds it; set before each mret into S-mode would be tidier,
	# but the two stages are distinguishable by mcounteren alone.
	csrr	t5, mcounteren
	beqz	t5, 2f
	la	t5, m_after_allowed
	j	3f
2:
	la	t5, m_after_denied
3:
	csrw	mepc, t5
	li	t5, 3 << 11		# MPP = M, so mret returns to M-mode
	csrs	mstatus, t5
	mret

#---------------------------------------------------------------------
	.section .data
	.align 7			# 128-byte aligned: guard, then a
cbz_area:				# 64-byte-aligned block at +64
	.fill	256, 1, 0

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
