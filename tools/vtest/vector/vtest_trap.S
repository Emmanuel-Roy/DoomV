# Differential test for trap delegation and the scause/stval a trap reports.
#
# Nothing else here checks what a trap actually *says*. The MMU test proves a
# fault-only-first load avoids trapping, but the values a real trap writes --
# which cause, what stval, whether it landed in S-mode at all -- have never
# been compared against a reference. Those values are what an OS branches on,
# so a wrong cause is the kind of bug that surfaces as the kernel handling
# entirely the wrong thing.
#
# Setup mirrors vtest_mmu.S: one 2MB identity megapage over
# 0x80000000-0x801fffff, nothing above it, then drop to S-mode. On top of
# that this delegates the relevant exceptions to S-mode via medeleg and
# installs an stvec handler that records scause/stval and steps over the
# faulting instruction.
#
# Deliberately NOT covered: illegal instructions. DoomV treats those as an
# unconditional debugger halt rather than a real trap (see exec_32ZICSR's
# comment) -- a deliberate choice that predates it having any trap handler
# to land in, and one that would make this test halt rather than compare.
# Worth revisiting now that a real OS runs on this, but it is a behaviour
# change, not a test gap.
#
# Also deliberately NOT covered: misaligned accesses. The spec lets an
# implementation either support them natively or raise an exception, and the
# two references here genuinely differ -- spike raises
# load-address-misaligned, DoomV performs the access. Both are legal, so a
# differential test on it compares a choice rather than a correctness
# property. (This was found by writing that test anyway: DoomV skipped the
# handler, which shifted every later result by one trap and made six
# unrelated lines look wrong. Linux agrees with DoomV's side of it, and even
# reports as much at boot: "unaligned accesses are fast".)

.section .text.start
.global _start
_start:
	la	sp, __stacktop

	# Enable FP/vector before leaving M-mode -- not used here, but the
	# enforcement added for VS/FS means anything that touches them in
	# S-mode would trap as illegal rather than doing what this test means.
	li	t0, (1 << 13) | (1 << 9)
	csrs	mstatus, t0

	#---- page table: identical to vtest_mmu.S ---------------------------
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
	sd	t3, 0(t2)

	srli	t3, t1, 12
	li	t4, 8
	slli	t4, t4, 60
	or	t3, t3, t4
	csrw	satp, t3
	sfence.vma

	#---- delegate the exceptions this test raises to S-mode -------------
	# Without this they would trap to M-mode instead, and the handler below
	# (installed in stvec) would never run at all.
	#   3  breakpoint
	#   4  load address misaligned
	#   6  store address misaligned
	#   9  ecall from S-mode
	#   13 load page fault
	#   15 store/AMO page fault
	li	t0, (1 << 3) | (1 << 4) | (1 << 6) | (1 << 9) | (1 << 13) | (1 << 15)
	csrw	medeleg, t0

	# Safety net. Anything that is not delegated lands in M-mode at mtvec,
	# which is zero unless set -- so pc goes to 0 and the run spins in
	# instruction-access faults forever. In debug mode that shows up as
	# spike producing no output at all, which is a genuinely confusing way
	# to discover you mis-delegated something. Terminate instead.
	la	t0, m_unexpected
	csrw	mtvec, t0

	la	t0, smode_entry
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11
	csrs	mstatus, t0
	mret

# Everything below runs in S-mode. norvc throughout: the handler steps over
# a faulting instruction by adding 4 to sepc, which is only correct if the
# assembler has not compressed it to 2 bytes.
.option push
.option norvc

	.align 4
trap_handler:
	csrr	t0, scause
	sd	t0, 0(s0)
	addi	s0, s0, 8
	csrr	t0, stval
	sd	t0, 0(s0)
	addi	s0, s0, 8
	# vstart too: for a faulting vector access it must name the element
	# that faulted, so the instruction can be restarted after the mapping
	# is fixed. Zero for every non-vector trap. Cleared afterwards so a
	# stale vstart does not make the *next* vector instruction skip
	# elements -- a real supervisor would restart the instruction instead.
	csrr	t0, vstart
	sd	t0, 0(s0)
	addi	s0, s0, 8
	csrw	vstart, x0
	csrr	t0, sepc
	addi	t0, t0, 4
	csrw	sepc, t0
	sret

	.align	4
m_unexpected:
	# Reached only if an exception was not delegated to S-mode. End the run
	# rather than hanging; the short signature makes it obvious.
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
1:	j	1b

smode_entry:
	la	s0, begin_signature
	la	t0, trap_handler
	csrw	stvec, t0			# direct mode: low bits clear

	#---- load page fault: read from the hole above the mapped page
	li	t1, 0x80200000
	ld	t2, 0(t1)			# -> scause 13, stval 0x80200000

	#---- store page fault at the same address
	sd	t2, 0(t1)			# -> scause 15, stval 0x80200000

	#---- a fault partway into a page still reports the exact address,
	#---- not the page base. Must stay 8-byte aligned: an unaligned ld
	#---- raises a misaligned-access exception before the MMU is ever
	#---- consulted, so an unaligned address here would test the wrong
	#---- thing entirely.
	li	t1, 0x80200ab8
	ld	t2, 0(t1)			# -> scause 13, stval 0x80200ab8


	#---- environment call from S-mode
	ecall					# -> scause 9, stval 0

	#---- breakpoint
	ebreak					# -> scause 3, stval = the ebreak's pc

	#---- a plain (non-FOF) vector load crossing out of the mapped page.
	#---- Unlike vle8ff.v this must genuinely trap -- and vstart must name
	#---- the element it stopped on, not 0, or the instruction could not be
	#---- restarted. 8 elements fit below the boundary, so element 8 faults.
	li	t1, 0x801ffff8
	vsetvli	t3, x0, e8, m1, ta, ma
	vle8.v	v1, (t1)		# -> scause 13, stval 0x80200000, vstart 8

	#---- confirm we are still in S-mode and running: a normal access to
	#---- mapped memory must still work after all that
	li	t1, 0x801ff000
	li	t2, 0x5a5a5a5a5a5a5a5a
	sd	t2, 0(t1)
	ld	t3, 0(t1)
	sd	t3, 0(s0)
	addi	s0, s0, 8

.option pop

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
