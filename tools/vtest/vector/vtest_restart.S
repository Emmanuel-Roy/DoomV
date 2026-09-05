# Differential test for a genuine restart of a faulting vector access.
#
# vtest_trap checks that a faulting vle8.v leaves vstart on the element it
# stopped at. That is necessary but not sufficient: it proves the value is
# recorded, not that re-executing the instruction actually resumes from
# there. This does the thing vstart exists for -- the handler maps the
# missing page and returns *without* advancing sepc, so the very same
# instruction runs again and finishes the elements it could not reach.
#
# What makes the result meaningful is the data. Elements 0..7 were loaded by
# the first, partial execution and must survive untouched (the restarted
# execution starts at vstart and never revisits them); elements 8..15 come
# from the page that did not exist the first time round. So the final
# register can only be correct if both halves happened, in order, across two
# executions of one instruction.
#
# Layout: two 2MB pages, 0x80000000 and 0x80200000. Both are mapped up front
# so the test data can be written, then the second is unmapped to set up the
# fault.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	li	t0, (1 << 13) | (1 << 9)		# FS/VS = Initial
	csrs	mstatus, t0

	#---- page table: two adjacent 2MB leaves, both valid for now
	la	t1, pgt_root
	la	t2, pgt_l1

	srli	t3, t2, 12
	slli	t3, t3, 10
	ori	t3, t3, 0x01
	addi	t4, t1, 2 * 8
	sd	t3, 0(t4)

	li	t3, 0x80000000
	srli	t3, t3, 12
	slli	t3, t3, 10
	ori	t3, t3, 0xcf
	sd	t3, 0(t2)				# l1[0] -> 0x80000000

	li	t3, 0x80200000
	srli	t3, t3, 12
	slli	t3, t3, 10
	ori	t3, t3, 0xcf
	sd	t3, 8(t2)				# l1[1] -> 0x80200000

	srli	t3, t1, 12
	li	t4, 8
	slli	t4, t4, 60
	or	t3, t3, t4
	csrw	satp, t3
	sfence.vma

	li	t0, (1 << 13) | (1 << 15)		# delegate load/store page faults
	csrw	medeleg, t0

	la	t0, m_unexpected
	csrw	mtvec, t0

	la	t0, smode_entry
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

.option push
.option norvc

	.align 4
m_unexpected:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
1:	j	1b

	.align 4
restart_handler:
	# Save every scratch register this handler touches. That is not
	# housekeeping -- it is the whole reason a restart can work. The faulting
	# instruction is `vle8.v v1, (t1)`, so if the handler leaves its own
	# value in t1 the re-executed load reads from the wrong base entirely.
	# (Getting this wrong the first time made element 8 come back holding
	# the page-table entry the handler had just written, which is a very
	# on-the-nose way to be told the base register was clobbered.)
	addi	sp, sp, -32
	sd	t0, 0(sp)
	sd	t1, 8(sp)
	sd	t2, 16(sp)

	# s2 counts faults. The first one maps the page and retries; a second
	# would mean the mapping did not take, so give up and step over the
	# instruction rather than faulting forever. s0/s2 are deliberately NOT
	# restored: the signature pointer and the counter must carry forward.
	addi	s2, s2, 1
	li	t0, 1
	bne	s2, t0, give_up

	# Record where the fault stopped, then map the page that was missing.
	csrr	t0, vstart
	sd	t0, 0(s0)
	addi	s0, s0, 8

	la	t1, pgt_l1
	li	t2, 0x80200000
	srli	t2, t2, 12
	slli	t2, t2, 10
	ori	t2, t2, 0xcf
	sd	t2, 8(t1)
	sfence.vma

	# No sepc advance: sret re-enters the same instruction, which resumes
	# from vstart rather than starting over.
	j	handler_ret

give_up:
	csrr	t0, sepc
	addi	t0, t0, 4
	csrw	sepc, t0

handler_ret:
	ld	t0, 0(sp)
	ld	t1, 8(sp)
	ld	t2, 16(sp)
	addi	sp, sp, 32
	sret

smode_entry:
	# Re-point the stack inside the mapped region. __stacktop is the top of
	# the 256MB RAM the linker script describes, which paging does not cover
	# -- only two 2MB pages exist here. M-mode did not care because
	# translation is off there, but the handler below pushes registers, and
	# a fault on that push would recurse forever.
	li	sp, 0x801f0000
	la	s0, begin_signature
	la	t0, restart_handler
	csrw	stvec, t0
	li	s2, 0					# fault counter

	#---- test data either side of the page boundary, written while both
	#---- pages are still mapped
	li	t1, 0x801ffff8
	li	t2, 0x0102030405060708
	sd	t2, 0(t1)
	li	t1, 0x80200000
	li	t2, 0x1112131415161718
	sd	t2, 0(t1)

	#---- now take the second page away
	la	t1, pgt_l1
	sd	x0, 8(t1)
	sfence.vma

	#---- one vector load spanning both pages. Faults at element 8, the
	#---- handler maps the page, and it resumes from there.
	li	t1, 0x801ffff8
	vsetvli	t3, x0, e8, m1, ta, ma
	vle8.v	v1, (t1)

	#---- how many faults it took (must be exactly 1)
	sd	s2, 0(s0)
	addi	s0, s0, 8

	#---- vstart must be back to 0 after the instruction finally completed
	csrr	t0, vstart
	sd	t0, 0(s0)
	addi	s0, s0, 8

	#---- and the register must hold all 16 bytes: the first half from the
	#---- partial execution, the second from after the restart
	vse8.v	v1, (s0)
	addi	s0, s0, 16

.option pop

.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

#---------------------------------------------------------------------
	.section .data
	.align 12
pgt_root:
	.fill	4096, 1, 0
	.align 12
pgt_l1:
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
