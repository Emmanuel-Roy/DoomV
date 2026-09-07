# Differential test for two-stage address translation -- H's third
# increment, and the part that makes a hypervisor a hypervisor.
#
# The guest's page tables no longer produce real addresses. Everything they
# produce is a *guest physical* address, which the second stage (hgatp,
# Sv39x4) then places into real memory. That includes every intermediate
# page-table pointer, not just the final address: a single guest load walks
# three levels, and each of those walks is itself an address the second
# stage has to translate before the PTE can even be read.
#
# Testing it needs the same trick as hlv, one level deeper. The guest's
# tables map a VA to a guest physical address; the G-stage maps that guest
# physical address somewhere else again. Three distinct pages hold three
# distinct values, so the answer says exactly which stages were applied:
#
#   value 0x1111...  the hypervisor's own mapping   (no guest translation)
#   value 0x2222...  guest table applied, G-stage not
#   value 0x3333...  both stages applied            <- the correct answer
#
# An implementation that skipped the second stage returns 0x2222... without
# faulting, which is why "does it work" cannot answer this and only the
# value can.
#
# Every table edit below is followed by both sfence.vma and hfence.gvma.
# That is not belt-and-braces: sfence.vma orders the VS-stage and S-stage
# translations only, and has no effect on G-stage entries -- those need
# hfence.gvma. DoomV re-walks the tables on every access and has no
# translation cache, so it sees an edit immediately either way and the
# distinction is invisible on it. Sail caches, correctly requires the right
# fence, and reported no fault where one was expected. The test was wrong,
# not the model.
#
# The other half is where a fault comes from. The guest's tables being happy
# while the G-stage refuses is the case the distinct guest-page-fault causes
# (20/21/23) exist to report -- it means the hypervisor has not backed that
# guest page, which is its problem to fix, not the guest kernel's.

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
	li	s3, 0			# most recent cause

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- three payload pages, one per possible answer
	li	t0, 0x80210000		# hypervisor's own mapping
	li	t1, 0x1111111111111111
	sd	t1, 0(t0)
	li	t0, 0x80211000		# guest-physical, if G-stage were skipped
	li	t1, 0x2222222222222222
	sd	t1, 0(t0)
	li	t0, 0x80212000		# where the G-stage actually sends it
	li	t1, 0x3333333333333333
	sd	t1, 0(t0)

	#---- Hypervisor table: identity 2MB leaf, plus VA 0x80220000 -> the
	#---- 0x1111 page, so an ordinary ld has a distinct answer.
	la	t1, hs_root
	la	t2, hs_l1
	la	t3, hs_l2
	srli	t4, t2, 12
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t1, 2 * 8
	sd	t4, 0(t5)
	li	t4, 0x80000000
	srli	t4, t4, 12
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	sd	t4, 0(t2)
	srli	t4, t3, 12
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t2, 1 * 8
	sd	t4, 0(t5)
	li	t4, 0x80210
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	addi	t5, t3, 0x20 * 8
	sd	t4, 0(t5)

	#---- Guest table: VA 0x80220000 -> guest physical 0x80211000.
	#----
	#---- The guest's tables are addressed by GUEST PHYSICAL address, not by
	#---- their real one. vsatp below points at GPA 0x80240000, and the
	#---- G-stage maps 0x80240/41/42 onto the real g_root/g_l1/g_l2 pages.
	#---- So walking the guest's tables requires the second stage on every
	#---- intermediate pointer, not merely on the final address.
	#----
	#---- That distinction is not academic: with the tables sitting in an
	#---- identity-mapped region, an implementation that never translates
	#---- intermediate PTE addresses gets the right answer anyway, and the
	#---- whole path goes untested. It was, until this mapping was added.
	la	t1, g_root
	la	t2, g_l1
	la	t3, g_l2
	li	t4, 0x80241		# GPA of g_l1, not its real PPN
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t1, 2 * 8
	sd	t4, 0(t5)
	li	t4, 0x80000000
	srli	t4, t4, 12
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	sd	t4, 0(t2)
	li	t4, 0x80242		# GPA of g_l2
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t2, 1 * 8
	sd	t4, 0(t5)
	li	t4, 0x80211		# guest physical, NOT a real address
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	addi	t5, t3, 0x20 * 8
	sd	t4, 0(t5)

	#---- G-stage table (Sv39x4). The root is four pages wide and its top
	#---- index is 11 bits, but for guest physical addresses below 2^39 the
	#---- extra index bits are zero and it indexes like Sv39.
	#---- Identity-map the 2MB region so the guest's tables and code
	#---- resolve, except for one 4KB hole handled below.
	la	t1, gs_root
	la	t2, gs_l1
	la	t3, gs_l2
	srli	t4, t2, 12
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t1, 2 * 8
	sd	t4, 0(t5)
	li	t4, 0x80000000		# 2MB identity leaf, U set: G-stage
	srli	t4, t4, 12		# pages must be user-accessible, since
	slli	t4, t4, 10		# every guest sits below HS
	ori	t4, t4, 0xdf		# D A U X W R V
	sd	t4, 0(t2)
	srli	t4, t3, 12		# second 2MB covered by a 4KB table
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t2, 1 * 8
	sd	t4, 0(t5)

	#---- Identity-map the whole 2MB the l2 table covers, so the guest's
	#---- own page tables and code resolve through the second stage. Then
	#---- override the one entry that matters.
	li	a1, 0x80200		# guest physical PPN of the first entry
	li	a2, 0			# table index
1:
	slli	t4, a1, 10
	ori	t4, t4, 0xdf		# D A U X W R V
	slli	t5, a2, 3
	add	t5, t3, t5
	sd	t4, 0(t5)
	addi	a1, a1, 1
	addi	a2, a2, 1
	li	a3, 512
	blt	a2, a3, 1b

	#---- Guest physical 0x80211000 -> real 0x80212000. This single
	#---- relocation is what distinguishes "both stages applied" from
	#---- "first stage only".
	li	t4, 0x80212
	slli	t4, t4, 10
	ori	t4, t4, 0xdf
	addi	t5, t3, 0x11 * 8
	sd	t4, 0(t5)

	#---- And place the guest's own page tables: GPA 0x80240/41/42 -> the
	#---- real g_root/g_l1/g_l2 pages, wherever the linker put them. These
	#---- three entries are what make the intermediate-PTE translation
	#---- load-bearing: without them the guest's tables sit in the
	#---- identity-mapped region and an implementation that never
	#---- translates a PTE address still reads the right PTEs.
	la	a1, g_root
	srli	a1, a1, 12
	slli	t4, a1, 10
	ori	t4, t4, 0xdf
	addi	t5, t3, 0x40 * 8
	sd	t4, 0(t5)
	la	a1, g_l1
	srli	a1, a1, 12
	slli	t4, a1, 10
	ori	t4, t4, 0xdf
	addi	t5, t3, 0x41 * 8
	sd	t4, 0(t5)
	la	a1, g_l2
	srli	a1, a1, 12
	slli	t4, a1, 10
	ori	t4, t4, 0xdf
	addi	t5, t3, 0x42 * 8
	sd	t4, 0(t5)

	#---- install all three roots
	la	t1, hs_root
	srli	t4, t1, 12
	li	t5, 8
	slli	t5, t5, 60
	or	t4, t4, t5
	csrw	satp, t4
	li	t4, 0x80240		# GPA of the guest's root table
	li	t5, 8
	slli	t5, t5, 60
	or	t4, t4, t5
	csrw	vsatp, t4
	la	t1, gs_root
	srli	t4, t1, 12
	li	t5, 8
	slli	t5, t5, 60
	or	t4, t4, t5
	csrw	hgatp, t4
	sfence.vma
	hfence.gvma		# G-stage tables are not covered by

	li	t0, 1 << 8		# SPVP = 1: guest accesses run as VS
	csrw	hstatus, t0

	#---- drop to HS-mode, where hlv lives
	la	t0, hs_body
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

hs_body:
	li	s1, 0x80220000

	#---- ordinary load: hypervisor's table only -> 0x1111...
	ld	t2, 0(s1)
	SIGX	t2

	#---- hlv.d: BOTH stages -> 0x3333... An implementation that applied
	#---- only the guest's table would return 0x2222... and not fault.
	hlv.d	t2, (s1)
	SIGX	t2

	#---- a store through both stages must land on the real page the
	#---- G-stage selected, not the guest physical one
	li	t1, 0x0102030405060708
	hsv.d	t1, (s1)
	li	t0, 0x80212000
	ld	t2, 0(t0)
	SIGX	t2			# the relocated page has it
	li	t0, 0x80211000
	ld	t2, 0(t0)
	SIGX	t2			# the guest-physical page is untouched

	#---- Now remove the G-stage mapping for that guest page. The guest's
	#---- own tables are unchanged and still perfectly valid, so this must
	#---- fault as a *guest* page fault, not an ordinary one.
	la	t3, gs_l2
	addi	t5, t3, 0x11 * 8
	sd	x0, 0(t5)
	sfence.vma
	hfence.gvma		# G-stage tables are not covered by
	li	s2, 0
	li	s3, 0
	hlv.d	t2, (s1)
	SIGX	s2			# 1
	SIGX	s3			# 21: load guest-page fault, not 13

	#---- a store to the same unmapped guest page
	li	s2, 0
	li	s3, 0
	hsv.d	t1, (s1)
	SIGX	s2			# 1
	SIGX	s3			# 23: store guest-page fault

	#---- restore it and confirm the access works again, so the fault above
	#---- was the mapping and not something sticky
	li	t4, 0x80212
	slli	t4, t4, 10
	ori	t4, t4, 0xdf
	sd	t4, 0(t5)
	sfence.vma
	hfence.gvma		# G-stage tables are not covered by
	li	s2, 0
	hlv.d	t2, (s1)
	SIGX	s2			# 0
	SIGX	t2			# the value written earlier

	#---- A G-stage page without the U bit is unreachable by any guest:
	#---- both VS and VU sit below HS, so the second stage always sees the
	#---- access as user.
	li	t4, 0x80212
	slli	t4, t4, 10
	ori	t4, t4, 0xcf		# D A X W R V -- no U
	sd	t4, 0(t5)
	sfence.vma
	hfence.gvma		# G-stage tables are not covered by
	li	s2, 0
	li	s3, 0
	hlv.d	t2, (s1)
	SIGX	s2			# 1
	SIGX	s3			# 21

	#---- restore
	li	t4, 0x80212
	slli	t4, t4, 10
	ori	t4, t4, 0xdf
	sd	t4, 0(t5)
	sfence.vma
	hfence.gvma		# G-stage tables are not covered by

	#---- With the guest's own paging off, its addresses ARE guest physical
	#---- ones and only the G-stage applies. 0x80211000 must then relocate
	#---- to the 0x3333 page via the same G-stage entry.
	csrw	vsatp, x0
	sfence.vma
	hfence.gvma		# G-stage tables are not covered by
	li	t0, 0x80211000
	li	s2, 0
	hlv.d	t2, (t0)
	SIGX	t2			# the relocated page's contents
	SIGX	s2			# no fault

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
	.align 4
trap_handler:
	csrr	t6, mcause
	mv	s3, t6
	li	t5, 9			# ecall from HS
	beq	t6, t5, to_done

	addi	s2, s2, 1
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

to_done:
	la	t5, m_done
	csrw	mepc, t5
	li	t5, 3 << 11
	csrs	mstatus, t5
	mret

	.section .data
	.align 12
hs_root:
	.fill	4096, 1, 0
	.align 12
hs_l1:
	.fill	4096, 1, 0
	.align 12
hs_l2:
	.fill	4096, 1, 0
	.align 12
g_root:
	.fill	4096, 1, 0
	.align 12
g_l1:
	.fill	4096, 1, 0
	.align 12
g_l2:
	.fill	4096, 1, 0
	# Sv39x4's root is four pages wide and 16KB-aligned.
	.align 14
gs_root:
	.fill	16384, 1, 0
	.align 12
gs_l1:
	.fill	4096, 1, 0
	.align 12
gs_l2:
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
