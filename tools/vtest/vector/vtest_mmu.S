# Differential test for Sv39 paging and fault-only-first's actual point.
#
# Everything else in this directory runs bare-metal in M-mode with satp=0,
# where every address translates and nothing can fault. That leaves two
# things untestable: the MMU itself, and the only interesting half of
# vle<eew>ff.v -- what happens when an element past the first would fault.
#
# So this test builds a page table, drops to S-mode, and deliberately runs a
# fault-only-first load off the end of the mapped region.
#
# The map is one 2MB megapage covering 0x80000000-0x801fffff, identity
# mapped, and nothing above it. So 0x80200000 is a hole, and a load that
# starts just below it crosses into unmapped memory partway through:
#
#     0x801ffff8  ... 0x801fffff   mapped   (elements 0..7)
#     0x80200000  ...              UNMAPPED (element 8 onwards)
#
# vle8ff.v from 0x801ffff8 with vl=16 must therefore come back with vl
# trimmed to 8 and no trap. A plain vle8.v there would fault instead, which
# is exactly the bug that made this worth implementing: glibc's strlen reads
# a vector at a time and relies on FOF to make reading past the end safe.

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	# Turn on FP and vector before leaving M-mode. mstatus.FS/VS are Off at
	# reset and DoomV now enforces that, so a vector instruction in S-mode
	# would otherwise be an illegal instruction rather than a page fault --
	# a very confusing way to fail this particular test.
	li	t0, (1 << 13) | (1 << 9)		# FS=Initial, VS=Initial
	csrs	mstatus, t0

	#---- build the page table ------------------------------------------
	# Sv39, VA 0x80000000: VPN[2]=2, VPN[1]=0. So the root's entry 2 points
	# at an L1 table, whose entry 0 is a 2MB leaf. Entry 1 is left zero,
	# which is what makes 0x80200000 fault.
	la	t1, pgt_root
	la	t2, pgt_l1

	srli	t3, t2, 12			# root[2] = PPN(pgt_l1) | V, non-leaf
	slli	t3, t3, 10
	ori	t3, t3, 0x01
	addi	t4, t1, 2 * 8
	sd	t3, 0(t4)

	li	t3, 0x80000000			# l1[0] = PPN(0x80000000) | D A X W R V
	srli	t3, t3, 12
	slli	t3, t3, 10
	ori	t3, t3, 0xcf
	sd	t3, 0(t2)

	srli	t3, t1, 12			# satp = MODE(8=Sv39) | PPN(root)
	li	t4, 8
	slli	t4, t4, 60
	or	t3, t3, t4
	csrw	satp, t3
	sfence.vma

	#---- drop to S-mode, where paging actually applies -----------------
	la	t0, smode_entry
	csrw	mepc, t0
	li	t0, 3 << 11			# clear MPP, then set it to S(01)
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

smode_entry:
	la	s0, begin_signature

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- Sv39 works at all: store and load back through a translated VA
	li	t0, 0x801ff000
	li	t1, 0x1122334455667788
	sd	t1, 0(t0)
	ld	t2, 0(t0)
	SIGX	t2

	#---- and a second address in the same page, to catch a map that
	#---- happens to work only at its base
	li	t0, 0x801ff800
	li	t1, 0x8877665544332211
	sd	t1, 0(t0)
	ld	t2, 0(t0)
	SIGX	t2

	#---- fault-only-first crossing out of the mapped page.
	# 8 known bytes sit at 0x801ffff8..0x801fffff; element 8 would come
	# from 0x80200000, which has no mapping.
	li	t0, 0x801ffff8
	li	t1, 0x0102030405060708
	sd	t1, 0(t0)
	vsetvli	t3, x0, e8, m1, ta, ma
	SIGX	t3				# vl before: VLMAX for e8/m1 = 16
	vle8ff.v v1, (t0)
	csrr	t4, vl
	SIGX	t4				# vl after: must be trimmed to 8

	#---- the same instruction wholly inside the mapped page must not
	#---- trim anything -- otherwise "trimmed to 8" above proves nothing
	li	t0, 0x801ff000
	vsetvli	t3, x0, e8, m1, ta, ma
	vle8ff.v v2, (t0)
	csrr	t4, vl
	SIGX	t4				# vl after: still 16

	#---- the bytes FOF did manage to load, to confirm it loaded the
	#---- right ones rather than merely stopping at the right index
	li	t0, 0x801ffff8
	vsetvli	t3, x0, e8, m1, ta, ma
	vle8ff.v v3, (t0)
	vsetvli	t3, x0, e64, m1, ta, ma
	vmv.x.s	t4, v3
	SIGX	t4

	#---- done
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
