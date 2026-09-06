# Differential test for the supervisor address-translation extensions:
# Svnapot, Svpbmt and Svinval.
#
# Svnapot and Svpbmt add no instructions at all -- they give meaning to bits
# of the page table entry that were previously reserved. That makes them
# untestable by any means except actually walking a page table, which is why
# this suite builds one and drops to S-mode rather than running bare like
# most of the others.
#
# The three things being checked are each invisible to a naive
# implementation in a different way:
#
#   * Svnapot's N bit means the low four bits of the PPN come from the
#     *virtual* address rather than the PTE, so one entry covers 64KB. An
#     implementation that ignores the bit still translates -- it just sends
#     every address in the range to the same wrong page. So the test writes
#     distinct values through the first and last page of a NAPOT range and
#     reads them back; if N were ignored they would alias onto each other.
#
#   * Svpbmt's memory-type field has no observable effect on a machine with
#     no caches: PMA, NC and IO all behave alike. What *is* observable is
#     the reserved value 3, and any nonzero value while menvcfg.PBMTE is
#     clear -- both must fault. That is how an OS probes for the extension,
#     so accepting them silently would claim support this hart lacks.
#
#   * Bits 60:54 of a PTE stay reserved and must fault if set. Without that
#     check the two above are meaningless, since a PTE with anything in the
#     high bits would translate happily either way.
#
# Svinval's three instructions have nothing to invalidate here -- the MMU
# walks the table in guest memory on every access, so a translation can
# never be stale. They are checked for retiring without disturbing anything.

.section .text.start
.global _start
_start:
	la	sp, __stacktop
	la	s0, begin_signature
	la	t0, trap_handler
	csrw	mtvec, t0
	li	s2, 0			# fault counter
	li	s3, 0			# first scause seen
	li	s4, 0			# stage marker, read by the trap handler

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- menvcfg.PBMTE enables Svpbmt for S-mode. Set it first; one case
	#---- below deliberately clears it again.
	li	t0, 1
	slli	t0, t0, 62
	csrs	menvcfg, t0

	#---- Build an Sv39 table.
	#  root[2]  -> L1                 (covers VA 0x80000000 upward)
	#  L1[0]    -> 2MB leaf           identity, 0x80000000-0x801fffff
	#  L1[1]    -> L2                 (covers 0x80200000-0x803fffff)
	#  L2[0..15] -> one NAPOT entry    64KB at 0x80200000
	#  L2[16]   -> plain 4KB leaf     0x80210000, used for the PBMT cases
	la	t1, pgt_root
	la	t2, pgt_l1
	la	t3, pgt_l2

	srli	t4, t2, 12		# root[2] = PPN(L1) | V
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t1, 2 * 8
	sd	t4, 0(t5)

	li	t4, 0x80000000		# L1[0] = 2MB leaf, D A X W R V
	srli	t4, t4, 12
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	sd	t4, 0(t2)

	srli	t4, t3, 12		# L1[1] = PPN(L2) | V
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t2, 1 * 8
	sd	t4, 0(t5)

	#---- the NAPOT entry: PPN of 0x80200000 with its low four bits set to
	#---- 0b1000, which is the only NAPOT size Sv39 defines (64KB), plus the
	#---- N bit in position 63.
	li	t4, 0x80200		# PPN
	ori	t4, t4, 0x8		# NAPOT size encoding
	slli	t4, t4, 10
	ori	t4, t4, 0xcf		# D A X W R V
	li	t5, 1
	slli	t5, t5, 63		# N
	or	t4, t4, t5
	#---- written into all sixteen slots the 64KB range spans
	mv	t6, t3
	li	a0, 16
1:
	sd	t4, 0(t6)
	addi	t6, t6, 8
	addi	a0, a0, -1
	bnez	a0, 1b

	#---- a plain 4KB leaf just above it, for the PBMT cases
	li	t4, 0x80210
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	addi	t5, t3, 16 * 8
	sd	t4, 0(t5)

	srli	t4, t1, 12		# satp = Sv39 | PPN(root)
	li	t5, 8
	slli	t5, t5, 60
	or	t4, t4, t5
	csrw	satp, t4
	sfence.vma

	#---- drop to S-mode
	la	t0, smode
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

smode:
	#---- Svnapot: distinct values through the first and last page of the
	#---- 64KB range. If the N bit were ignored, both would land on the
	#---- same physical page and the second store would overwrite the first.
	li	t0, 0x80200000
	li	t1, 0x1111111111111111
	sd	t1, 0(t0)
	li	t0, 0x8020F000		# last 4KB page of the NAPOT range
	li	t1, 0x2222222222222222
	sd	t1, 0(t0)
	li	t0, 0x80200000
	ld	t2, 0(t0)
	SIGX	t2			# must still be 0x1111...
	li	t0, 0x8020F000
	ld	t2, 0(t0)
	SIGX	t2			# must be 0x2222...

	#---- a middle page of the same range, to confirm the VA bits really
	#---- index within it rather than only the two ends working
	li	t0, 0x80207000
	li	t1, 0x3333333333333333
	sd	t1, 0(t0)
	ld	t2, 0(t0)
	SIGX	t2
	li	t0, 0x80200000
	ld	t2, 0(t0)
	SIGX	t2			# and the first page is still intact

	#---- Svinval: nothing to invalidate, but the instructions must retire
	#---- and must not disturb the mapping they nominally act on.
	sfence.w.inval
	li	t0, 0x80200000
	sinval.vma t0, x0
	sinval.vma x0, x0
	sfence.inval.ir
	ld	t2, 0(t0)
	SIGX	t2			# unchanged after the invalidation sequence
	SIGX	s2			# and no faults taken: must be 0

	#---- back to M-mode to rewrite PTEs for the Svpbmt cases
	ecall

m_pbmt:
	#---- PBMT = 1 (NC) on the plain leaf. With menvcfg.PBMTE set this is
	#---- accepted, and on a machine with no caches it behaves as PMA.
	la	t3, pgt_l2
	addi	t5, t3, 16 * 8
	li	t4, 0x80210
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	li	t6, 1
	slli	t6, t6, 61		# PBMT = 1 (NC)
	or	t4, t4, t6
	sd	t4, 0(t5)
	sfence.vma
	li	s4, 1			# stage marker for the trap handler
	la	t0, s_pbmt_nc
	j	enter_smode

s_pbmt_nc:
	li	t0, 0x80210000
	li	t1, 0x4444444444444444
	sd	t1, 0(t0)
	ld	t2, 0(t0)
	SIGX	t2			# must work: NC is a valid type
	SIGX	s2			# faults so far: still 0
	ecall

m_pbmt_reserved:
	#---- PBMT = 3 is reserved and must fault even with PBMTE set
	la	t3, pgt_l2
	addi	t5, t3, 16 * 8
	li	t4, 0x80210
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	li	t6, 3
	slli	t6, t6, 61
	or	t4, t4, t6
	sd	t4, 0(t5)
	sfence.vma
	li	s2, 0
	li	s3, 0
	li	s4, 2
	la	t0, s_pbmt_res
	j	enter_smode

s_pbmt_res:
	li	t0, 0x80210000
	ld	t2, 0(t0)		# must fault
	SIGX	s2			# fault count: must be 1
	SIGX	s3			# scause: load page fault = 13
	ecall

m_pbmt_disabled:
	#---- PBMT = 1 again, but with menvcfg.PBMTE cleared. The bits are then
	#---- reserved, so this must fault too -- which is exactly how an OS
	#---- discovers the extension is unavailable.
	la	t3, pgt_l2
	addi	t5, t3, 16 * 8
	li	t4, 0x80210
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	li	t6, 1
	slli	t6, t6, 61
	or	t4, t4, t6
	sd	t4, 0(t5)
	li	t0, 1
	slli	t0, t0, 62
	csrc	menvcfg, t0		# PBMTE = 0
	sfence.vma
	li	s2, 0
	li	s3, 0
	li	s4, 3
	la	t0, s_pbmt_off
	j	enter_smode

s_pbmt_off:
	li	t0, 0x80210000
	ld	t2, 0(t0)		# must fault: PBMT nonzero, PBMTE clear
	SIGX	s2			# 1
	SIGX	s3			# 13
	ecall

m_reserved_bits:
	#---- bit 54 set: reserved in every Sv mode, must fault
	la	t3, pgt_l2
	addi	t5, t3, 16 * 8
	li	t4, 0x80210
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	li	t6, 1
	slli	t6, t6, 54
	or	t4, t4, t6
	sd	t4, 0(t5)
	sfence.vma
	li	s2, 0
	li	s3, 0
	li	s4, 4
	la	t0, s_reserved
	j	enter_smode

s_reserved:
	li	t0, 0x80210000
	ld	t2, 0(t0)		# must fault
	SIGX	s2			# 1
	SIGX	s3			# 13
	ecall

m_bad_napot:
	#---- N set with a PPN whose low bits are not the 64KB encoding. Sv39
	#---- defines exactly one NAPOT size, so anything else is reserved.
	la	t3, pgt_l2
	li	t4, 0x80200
	ori	t4, t4, 0x4		# not 0b1000
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	li	t6, 1
	slli	t6, t6, 63
	or	t4, t4, t6
	sd	t4, 0(t3)
	sfence.vma
	li	s2, 0
	li	s3, 0
	li	s4, 5
	la	t0, s_bad_napot
	j	enter_smode

s_bad_napot:
	li	t0, 0x80200000
	ld	t2, 0(t0)		# must fault
	SIGX	s2			# 1
	SIGX	s3			# 13
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

#---------------------------------------------------------------------
# Enter S-mode at the address in t0.
enter_smode:
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

# M-mode trap handler. An ecall from S-mode advances the test to the next
# stage; anything else is a fault under test -- counted, its cause recorded,
# and the offending instruction skipped.
	.align 4
trap_handler:
	csrr	t6, mcause
	li	t5, 9			# ecall from S-mode
	beq	t6, t5, next_stage

	addi	s2, s2, 1
	bnez	s3, 1f
	mv	s3, t6
1:
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

next_stage:
	li	t5, 3 << 11		# return to M-mode
	csrs	mstatus, t5
	beqz	s4, 1f
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
1:	la	t5, m_pbmt
	j	9f
2:	la	t5, m_pbmt_reserved
	j	9f
3:	la	t5, m_pbmt_disabled
	j	9f
4:	la	t5, m_reserved_bits
	j	9f
5:	la	t5, m_bad_napot
9:
	csrw	mepc, t5
	mret

#---------------------------------------------------------------------
	.section .data
	.align 12
pgt_root:
	.fill	4096, 1, 0
	.align 12
pgt_l1:
	.fill	4096, 1, 0
	.align 12
pgt_l2:
	.fill	4096, 1, 0

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
