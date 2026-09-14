# Differential test for hlv/hsv -- the hypervisor's explicit guest accesses,
# and H's second increment.
#
# These instructions exist because a hypervisor emulating a guest
# instruction has to read the guest's operands *as the guest would*: through
# the guest's page tables, at the guest's privilege, faulting where the
# guest would fault. Dereferencing the address directly would use the
# hypervisor's own mappings instead, which is both a wrong answer and a
# privilege-escalation hole.
#
# That is exactly what makes them hard to test convincingly. A naive
# implementation that ignores the guest's translation still *works* whenever
# the two mappings agree. So this suite deliberately maps the same virtual
# address to two different physical pages -- one in the hypervisor's table,
# one in the guest's -- and checks which page hlv reaches. An implementation
# that used satp instead of vsatp reads the hypervisor's page and returns a
# different value; nothing faults, so only the value distinguishes them.
#
# hgatp stays 0 throughout. With the second stage bare, a two-stage walk
# reduces exactly to the guest's single-stage walk, which is what lets this
# increment be checked against spike before hgatp is implemented.

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

	#---- Two payloads at two different physical pages. The hypervisor's
	#---- table will map the test VA to one, the guest's to the other.
	li	t0, 0x80210000		# the page the HYPERVISOR maps
	li	t1, 0x1111111111111111
	sd	t1, 0(t0)
	li	t0, 0x80211000		# the page the GUEST maps
	li	t1, 0x2222222222222222
	sd	t1, 0(t0)

	#---- Hypervisor page table: identity 2MB leaf for the low region, plus
	#---- a 4KB leaf mapping VA 0x80220000 -> PA 0x80210000.
	la	t1, hs_root
	la	t2, hs_l1
	la	t3, hs_l2
	srli	t4, t2, 12		# root[2] -> L1
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t1, 2 * 8
	sd	t4, 0(t5)
	li	t4, 0x80000000		# L1[0] = 2MB identity leaf
	srli	t4, t4, 12
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	sd	t4, 0(t2)
	srli	t4, t3, 12		# L1[1] -> L2 (covers 0x80200000+)
	slli	t4, t4, 10
	ori	t4, t4, 0x01
	addi	t5, t2, 1 * 8
	sd	t4, 0(t5)
	li	t4, 0x80210		# L2[0x20] : VA 0x80220000 -> PA 0x80210000
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	addi	t5, t3, 0x20 * 8
	sd	t4, 0(t5)

	#---- Guest page table: the same VA 0x80220000 -> PA 0x80211000, the
	#---- other page. Plus the identity leaf so the guest can run at all.
	la	t1, g_root
	la	t2, g_l1
	la	t3, g_l2
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
	li	t4, 0x80211		# the guest's different mapping
	slli	t4, t4, 10
	ori	t4, t4, 0xcf
	addi	t5, t3, 0x20 * 8
	sd	t4, 0(t5)

	#---- a second guest mapping with no U bit, to test the guest-privilege
	#---- check: VA 0x80221000 -> PA 0x80211000, supervisor-only
	li	t4, 0x80211
	slli	t4, t4, 10
	ori	t4, t4, 0xc7		# D A X W R V, no U
	addi	t5, t3, 0x21 * 8
	sd	t4, 0(t5)

	#---- install both satps: the hypervisor's in satp, the guest's in vsatp
	la	t1, hs_root
	srli	t4, t1, 12
	li	t5, 8
	slli	t5, t5, 60
	or	t4, t4, t5
	csrw	satp, t4
	la	t1, g_root
	srli	t4, t1, 12
	li	t5, 8
	slli	t5, t5, 60
	or	t4, t4, t5
	csrw	vsatp, t4
	csrw	hgatp, x0		# second stage bare
	sfence.vma

	#---- SPVP = 1: the guest is in VS-mode, so hlv runs at supervisor
	#---- permission within the guest's table
	li	t0, 1 << 8
	csrw	hstatus, t0

	#---- Drop to HS-mode. hlv/hsv are hypervisor instructions; M-mode may
	#---- issue them too, but HS is where a hypervisor actually runs.
	la	t0, hs_body
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11		# MPP = S
	csrs	mstatus, t0
	mret

hs_body:
	li	s1, 0x80220000		# the VA both tables map, differently

	#---- An ordinary load uses the HYPERVISOR's table -> 0x1111...
	ld	t2, 0(s1)
	SIGX	t2

	#---- hlv.d uses the GUEST's table -> 0x2222... The two values differing
	#---- is the entire property being tested: same address, same
	#---- instruction stream, different translation.
	hlv.d	t2, (s1)
	SIGX	t2

	#---- the narrow forms, and their signedness. The guest page holds
	#---- 0x2222222222222222, so every byte is 0x22 and sign extension is
	#---- not visible; write a value with the high bit set in each width.
	li	t0, 0x80211000
	li	t1, 0xF0E0D0C0B0A09080
	sd	t1, 0(t0)

	hlv.b	t2, (s1)
	SIGX	t2			# 0x80 sign-extended
	hlv.bu	t2, (s1)
	SIGX	t2			# 0x80 zero-extended
	hlv.h	t2, (s1)
	SIGX	t2			# 0x9080 sign-extended
	hlv.hu	t2, (s1)
	SIGX	t2
	hlv.w	t2, (s1)
	SIGX	t2			# 0xB0A09080 sign-extended
	hlv.wu	t2, (s1)
	SIGX	t2
	hlv.d	t2, (s1)
	SIGX	t2

	#---- hsv writes through the guest's table too. Write via hsv, then
	#---- read the guest's physical page directly to confirm where it went.
	li	t1, 0x0102030405060708
	hsv.d	t1, (s1)
	li	t0, 0x80211000
	ld	t2, 0(t0)
	SIGX	t2			# the guest's page holds the new value
	li	t0, 0x80210000
	ld	t2, 0(t0)
	SIGX	t2			# the hypervisor's page is untouched

	#---- narrower stores
	li	t0, 0x80211000
	sd	x0, 0(t0)
	li	t1, 0xAABBCCDD
	hsv.b	t1, (s1)
	ld	t2, 0(t0)
	SIGX	t2			# only the low byte written
	hsv.h	t1, (s1)
	ld	t2, 0(t0)
	SIGX	t2
	hsv.w	t1, (s1)
	ld	t2, 0(t0)
	SIGX	t2

	#---- The guest-privilege check. SPVP=1 means the access runs as VS
	#---- (supervisor), which may reach a page without the U bit.
	li	s4, 0x80221000		# the supervisor-only guest mapping
	li	s2, 0
	hlv.d	t2, (s4)
	SIGX	s2			# no fault: 0

	#---- With SPVP=0 the access runs as VU (user), and a page without U
	#---- must now fault. Same instruction, same address -- only the
	#---- guest's privilege changed.
	csrw	hstatus, x0		# SPVP = 0
	li	s2, 0
	li	s3, 0
	hlv.d	t2, (s4)
	SIGX	s2			# 1
	SIGX	s3			# 13: load page fault

	#---- and the U-bit page is still reachable as VU
	li	t0, 1 << 8
	csrw	hstatus, t0		# SPVP = 1 again for the rest
	li	s2, 0
	hlv.d	t2, (s1)
	SIGX	s2			# 0

	#---- hfence.vvma/gvma have nothing to invalidate here, but must retire
	#---- and leave the mapping they nominally act on alone
	hfence.vvma x0, x0
	hfence.gvma x0, x0
	hlv.d	t2, (s1)
	SIGX	t2			# unchanged after the fences
	SIGX	s2			# still no faults

	ecall				# back to M-mode

m_done:
	#---- a guest may not issue hlv at all: it is a virtual instruction,
	#---- not an illegal one, so the hypervisor can emulate it
	li	t0, 1 << 7		# hstatus.SPV = 1 -> sret enters VS
	csrs	hstatus, t0
	la	t0, guest_body
	csrw	sepc, t0
	li	t0, 1 << 8		# SPP = S
	csrs	mstatus, t0
	la	t0, hs_to_guest
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

hs_to_guest:
	sret

guest_body:
	li	s2, 0
	li	s3, 0
	hlv.d	t2, (s1)
	SIGX	s2			# 1
	SIGX	s3			# 22: virtual instruction
	ecall

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
	li	t5, 9			# ecall from HS -> the guest stage
	beq	t6, t5, to_m_done
	li	t5, 10			# ecall from VS -> finish
	beq	t6, t5, to_sig_done

	addi	s2, s2, 1
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

to_m_done:
	la	t5, m_done
	csrw	mepc, t5
	li	t5, 3 << 11
	csrs	mstatus, t5
	mret

to_sig_done:
	la	t5, sig_done
	csrw	mepc, t5
	li	t5, 3 << 11
	csrs	mstatus, t5
	li	t5, 1
	slli	t5, t5, 39		# clear MPV or the mret re-enters the guest
	csrc	mstatus, t5
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
