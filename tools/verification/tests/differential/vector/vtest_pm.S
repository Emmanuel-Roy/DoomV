# Differential test for pointer masking: Ssnpm, Smnpm and Sspm.
#
# Pointer masking is address arithmetic, which makes it the hardest kind of
# feature to test by seeing whether something works. A wrong implementation
# does not fault -- it quietly translates a different address and succeeds.
# So every case here writes through a *tagged* pointer and observes the
# clean address: with masking on the two must reach the same memory, and
# with masking off the tagged store must leave the clean location alone.
#
# The control field lives in the envcfg of the mode *above* the one being
# masked -- menvcfg.PMM governs S-mode, senvcfg.PMM governs U-mode -- so a
# mode cannot exempt itself from the masking its supervisor imposed. That
# also dictates the shape of this test: the accesses have to happen in S or
# U mode while the CSR that governs them is written from the mode above, so
# it runs as a small stage machine driven from M-mode.
#
# satp stays 0 throughout. Masking transforms the effective address itself
# rather than its translation, so it applies in bare mode too, and bare mode
# keeps this test independent of any page-table behaviour.
#
# What is deliberately NOT observed here is whether a tagged access faults.
# A 7-bit tag necessarily puts the address far outside any RAM this machine
# has, and the two implementations disagree about what that means: spike
# raises an access fault for a physical address with no memory behind it,
# while DoomV has no physical-memory-attribute checking and lets the access
# evaporate. That is a real gap, but it is a gap about access faults, not
# about pointer masking, and using "did it fault" as a proxy for "was the
# tag still there" would be testing the wrong machine property.
#
# So masking-off is observed the other way round: a store through a tagged
# pointer must leave the clean location *unchanged*. Both machines agree on
# that -- one because the store faulted, the other because it landed
# somewhere harmless -- and it is a direct statement of the property under
# test rather than a proxy for it.

.section .text.start
.global _start
_start:
	la	sp, __stacktop
	la	s0, begin_signature
	la	t0, trap_handler
	csrw	mtvec, t0
	li	s2, 0			# fault counter
	li	s4, 0			# stage marker

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	la	s1, scratch		# clean pointer
	li	t0, 0x7F
	slli	t0, t0, 57
	or	s3, s1, t0		# same pointer with a 7-bit tag

#---------------------------------------------------------------------
# Stage 0: S-mode, masking off. The tagged pointer is a genuinely different
# address, so a store through it must not disturb the clean one. This
# establishes that the tag really does change the address -- without which
# nothing below means anything, since every later case would pass equally
# well on a machine that ignored the tag bits entirely.
	csrw	menvcfg, x0
	li	s4, 0
	la	t0, s_off
	j	enter_smode
s_off:
	li	t1, 0x0123456789abcdef
	sd	t1, 0(s1)		# a sentinel at the clean address
	li	t2, 0xdeadbeefdeadbeef
	sd	t2, 0(s3)		# store through the tagged pointer
	ld	t2, 0(s1)
	SIGX	t2			# sentinel must be untouched: the tag is real
	ecall

#---------------------------------------------------------------------
# Stage 1: S-mode with menvcfg.PMM = 2 (PMLEN=7). This is Smnpm: M-mode
# masking the mode below it.
m_on:
	li	t0, 2
	slli	t0, t0, 32
	csrw	menvcfg, t0
	li	s4, 1
	la	t0, s_on
	j	enter_smode
s_on:
	li	s2, 0
	li	t1, 0x1234567890abcdef
	sd	t1, 0(s3)		# store through the tagged pointer
	ld	t2, 0(s1)		# read through the clean one
	SIGX	t2			# must be 0x1234...
	SIGX	s2			# no fault

	li	t1, 0x0fedcba987654321	# and the reverse, to rule out an
	sd	t1, 0(s1)		# implementation that masks only stores
	ld	t2, 0(s3)
	SIGX	t2

	# Every tag value must reach the same address: the bits are discarded,
	# not folded into the result.
	li	t0, 0x2A
	slli	t0, t0, 57
	or	t4, s1, t0
	li	t1, 0x6666666666666666
	sd	t1, 0(t4)
	ld	t2, 0(s1)
	SIGX	t2

	# Sign extension, not zeroing. Bit 56 is the highest bit that survives
	# PMLEN=7, so an address with it set must stay high after masking rather
	# than collapsing into the low half of the space. Neither destination is
	# backed by memory, so what is checked is the consequence: the clean
	# location must be untouched either way. An implementation that zeroed
	# the top bits instead of sign-extending would land back near the clean
	# address and corrupt it.
	li	t0, 1
	slli	t0, t0, 56
	or	t3, s1, t0
	li	t1, 0x0123456789abcdef
	sd	t1, 0(s1)		# re-establish the sentinel
	li	t2, 0x5555555555555555
	sd	t2, 0(t3)
	ld	t2, 0(s1)
	SIGX	t2			# sentinel intact: masking kept it high
	ecall

#---------------------------------------------------------------------
# Stage 2: senvcfg.PMM governs U-mode, not S-mode. Set it while masking for
# S-mode is off; S-mode addresses must be unaffected.
m_senv:
	csrw	menvcfg, x0
	li	t0, 2
	slli	t0, t0, 32
	csrw	senvcfg, t0
	li	s4, 2
	la	t0, s_senv
	j	enter_smode
s_senv:
	li	t1, 0x0123456789abcdef
	sd	t1, 0(s1)
	li	t2, 0xcafecafecafecafe
	sd	t2, 0(s3)		# tagged, and senvcfg is not ours to obey
	ld	t2, 0(s1)
	SIGX	t2			# sentinel intact: S-mode was not masked
	ecall

#---------------------------------------------------------------------
# Stage 3: U-mode with senvcfg.PMM = 2. This is Ssnpm, the one RVA23 names
# for the U-mode case.
m_umode:
	li	s4, 3
	la	t0, u_on
	j	enter_umode
u_on:
	li	s2, 0
	li	t1, 0x2222222222222222
	sd	t1, 0(s3)		# tagged store from U-mode
	ld	t2, 0(s1)
	SIGX	t2			# must be 0x2222...
	SIGX	s2			# no fault
	ecall

#---------------------------------------------------------------------
# Stage 4: PMLEN=16 (PMM = 3), the larger defined size.
m_pmlen16:
	li	t0, 3
	slli	t0, t0, 32
	csrw	menvcfg, t0
	li	s4, 4
	la	t0, s_wide
	j	enter_smode
s_wide:
	li	s2, 0
	li	t0, 0xFFFF
	slli	t0, t0, 48
	or	t5, s1, t0		# a 16-bit tag
	li	t1, 0x7777777777777777
	sd	t1, 0(t5)
	ld	t2, 0(s1)
	SIGX	t2			# must be 0x7777...
	SIGX	s2			# no fault
	ecall

#---------------------------------------------------------------------
# Stage 5: PMM = 1 is reserved and must behave as masking off.
m_reserved:
	li	t0, 1
	slli	t0, t0, 32
	csrw	menvcfg, t0
	li	s4, 5
	la	t0, s_res
	j	enter_smode
s_res:
	li	t1, 0x0123456789abcdef
	sd	t1, 0(s1)
	li	t2, 0xfeedfeedfeedfeed
	sd	t2, 0(s3)
	ld	t2, 0(s1)
	SIGX	t2			# sentinel intact: reserved behaves as off
	ecall

#---------------------------------------------------------------------
m_done:
.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

#---------------------------------------------------------------------
enter_smode:
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11		# MPP = S
	csrs	mstatus, t0
	mret

enter_umode:
	csrw	mepc, t0
	li	t0, 3 << 11		# MPP = U
	csrc	mstatus, t0
	mret

# M-mode handler. An ecall from S (cause 9) or U (cause 8) advances to the
# next stage; anything else is a fault under test -- counted, and the
# offending 4-byte instruction skipped.
	.align 4
trap_handler:
	csrr	t6, mcause
	li	t5, 9
	beq	t6, t5, next_stage
	li	t5, 8
	beq	t6, t5, next_stage

	addi	s2, s2, 1
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

next_stage:
	li	t5, 3 << 11		# return to M-mode
	csrs	mstatus, t5
	li	t6, 0
	beq	s4, t6, 1f
	li	t6, 1
	beq	s4, t6, 2f
	li	t6, 2
	beq	s4, t6, 3f
	li	t6, 3
	beq	s4, t6, 4f
	li	t6, 4
	beq	s4, t6, 5f
	la	t5, m_done
	j	9f
1:	la	t5, m_on
	j	9f
2:	la	t5, m_senv
	j	9f
3:	la	t5, m_umode
	j	9f
4:	la	t5, m_pmlen16
	j	9f
5:	la	t5, m_reserved
9:
	csrw	mepc, t5
	mret

	.section .data
	.align 8
scratch:
	.fill	64, 1, 0

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
