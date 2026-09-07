# Differential test for the hypervisor extension's CSR file and privilege
# plumbing -- the first increment of H, before two-stage translation,
# hlv/hsv or htinst exist.
#
# The property being tested is that virtualisation is orthogonal to
# privilege rather than another level of it. The hart is in a (privilege,
# virtual) pair, and three consequences follow that this suite checks:
#
#   * In VS-mode the S-mode CSR *names* stop referring to the S-mode CSRs.
#     A guest writing "stvec" is writing vstvec, and cannot reach the real
#     stvec at all. The test writes distinct values to stvec from HS-mode
#     and from VS-mode and confirms neither disturbs the other -- which is
#     the whole isolation guarantee in one check.
#
#   * A guest touching the hypervisor's own registers raises a *virtual
#     instruction* exception (cause 22), not an illegal instruction (cause
#     2). The difference is what lets a hypervisor emulate the access on
#     the guest's behalf rather than killing it, so the cause number is
#     tested, not merely the fact that something trapped.
#
#   * V is saved and restored around traps. hstatus.SPV records it for a
#     trap into HS-mode and mstatus.MPV for one into M, and the matching
#     xRET restores it. Getting this wrong resumes the guest's code at the
#     hypervisor's own privilege level, which is a containment failure
#     rather than a wrong answer.
#
# Everything runs with satp and hgatp at 0, so no translation happens and
# this suite stays independent of the two-stage walk added later.

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
	li	s3, 0			# cause of the most recent trap
	li	s4, 0			# stage marker

.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- misa.H must advertise the extension
	csrr	t0, misa
	srli	t0, t0, 7		# bit 7 == 'H' - 'A'
	andi	t0, t0, 1
	SIGX	t0

	#---- hstatus: VSXL reads as 2 (64-bit VS mode) and is not writable,
	#---- and the reserved bits must read back as zero rather than as
	#---- whatever was written. A hypervisor probes its own capabilities
	#---- this way, so a register that echoes writes tells it lies.
	#---- VGEIN (bits 17:12) is excluded from the comparison. Its width is
	#---- GEILEN, the number of guest external interrupts the platform
	#---- wires up -- a configuration choice, not an architectural one.
	#---- DoomV has no guest interrupt file at all, so GEILEN is 0 and the
	#---- field is correctly read-only zero; a reference model configured
	#---- with guest interrupts holds a nonzero value there and is equally
	#---- correct. Diffing it would compare the two platforms, not the two
	#---- implementations of the architecture.
	li	t0, -1
	csrw	hstatus, t0
	csrr	t1, hstatus
	li	t2, 0x3F << 12
	not	t2, t2
	and	t1, t1, t2
	SIGX	t1
	csrw	hstatus, x0
	csrr	t1, hstatus
	SIGX	t1			# VSXL still 2 with everything else clear

	#---- the VS shadows are ordinary read/write storage from HS-mode
	li	t0, 0x1234
	csrw	vstvec, t0
	csrr	t1, vstvec
	SIGX	t1
	li	t0, 0x5678
	csrw	vsscratch, t0
	csrr	t1, vsscratch
	SIGX	t1

	#---- Set up distinguishable S-mode and VS-mode state. stvec belongs to
	#---- the hypervisor; vstvec is what the guest will see under that name.
	la	t0, hs_stvec_target
	csrw	stvec, t0
	li	t0, 0x0BADC0DE
	csrw	vstvec, t0
	li	t0, 0x1111
	csrw	sscratch, t0
	li	t0, 0x2222
	csrw	vsscratch, t0

	#---- enter the guest: hstatus.SPV = 1 means the following SRET goes to
	#---- VS-mode rather than plain S-mode.
	li	t0, 1 << 7		# SPV
	csrw	hstatus, t0
	la	t0, guest_entry
	csrw	sepc, t0
	li	t0, 1 << 8		# SPP = S, so the guest lands in VS not VU
	csrs	mstatus, t0
	li	s4, 0
	# Drop to HS-mode first: SRET is what enters a guest, and it has to be
	# executed from HS. Getting there is an MRET.
	la	t0, hs_enter_guest
	csrw	mepc, t0
	li	t0, 3 << 11
	csrc	mstatus, t0
	li	t0, 1 << 11		# MPP = S
	csrs	mstatus, t0
	mret

hs_enter_guest:
	sret				# -> guest_entry, in VS-mode

guest_entry:
	#---- The guest writes what it thinks is stvec. It must land in vstvec
	#---- and leave the hypervisor's stvec untouched.
	li	t0, 0x0FEEDBEE
	csrw	stvec, t0
	csrr	t1, stvec
	SIGX	t1			# reads back its own write: 0x0FEEDBEE

	#---- and the same for sscratch, to show it is the whole S-mode set
	#---- being shadowed rather than one special-cased register
	li	t0, 0x3333
	csrw	sscratch, t0
	csrr	t1, sscratch
	SIGX	t1

	#---- Touching the hypervisor's own registers is a virtual instruction
	#---- exception, not an illegal one.
	li	s2, 0
	li	s3, 0
	csrr	t1, hstatus
	SIGX	s2			# trap count: 1
	SIGX	s3			# cause: 22, not 2

	#---- reaching the VS registers by their own names is equally denied:
	#---- the guest must go through the S-mode names
	li	s2, 0
	li	s3, 0
	csrr	t1, vstvec
	SIGX	s2			# 1
	SIGX	s3			# 22

	#---- hgatp too, which the later two-stage work will make load-bearing
	li	s2, 0
	li	s3, 0
	csrr	t1, hgatp
	SIGX	s2			# 1
	SIGX	s3			# 22

	#---- An ordinary illegal instruction must still be cause 2, so the two
	#---- causes are genuinely distinguished rather than one having replaced
	#---- the other. A *decoder*-level illegal encoding cannot be used for
	#---- this: DoomV pauses for the operator on one rather than trapping
	#---- (F9 resumes and delivers the trap), which a batch run would sit
	#---- on until it timed out. A write to a read-only CSR is an illegal
	#---- instruction raised from the CSR path instead, which traps
	#---- normally and is just as much cause 2.
	li	s2, 0
	li	s3, 0
	csrw	cycle, t0		# read-only CSR: illegal, cause 2
	SIGX	s2			# 1
	SIGX	s3			# 2, not 22

	#---- back to M-mode. The ecall from VS-mode has its own cause (10),
	#---- distinct from an ecall from HS (9) or U (8) -- another thing a
	#---- hypervisor needs in order to know who called it.
	ecall

m_after_guest:
	SIGX	s3			# cause of the ecall: 10 (from VS)

	#---- The trap out of the guest must have recorded where it came from:
	#---- mstatus.MPV = 1 (it was virtual) and MPP = S. s5 holds mstatus as
	#---- the trap left it, captured before the handler altered it.
	mv	t0, s5
	srli	t1, t0, 39
	andi	t1, t1, 1
	SIGX	t1			# MPV: 1
	srli	t1, t0, 11
	andi	t1, t1, 3
	SIGX	t1			# MPP: 1 (S)

	#---- and the hypervisor's own registers survived the guest entirely
	csrr	t1, sscratch
	SIGX	t1			# still 0x1111, not the guest's 0x3333
	csrr	t1, vsscratch
	SIGX	t1			# the guest's write landed here: 0x3333
	csrr	t1, vstvec
	SIGX	t1			# 0x0FEEDBEE

	#---- stvec still points at the hypervisor's handler, which is the
	#---- containment guarantee stated as a value rather than a behaviour
	la	t0, hs_stvec_target
	csrr	t1, stvec
	sub	t1, t1, t0
	SIGX	t1			# 0 if unchanged

.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

hs_stvec_target:
	j	hs_stvec_target		# never reached: nothing delegates to S here

#---------------------------------------------------------------------
# M-mode handler. An ecall from VS (cause 10) ends the guest stage; every
# other trap is one under test -- counted, its cause recorded, and the
# 4-byte instruction skipped.
	.align 4
trap_handler:
	csrr	t6, mcause
	mv	s3, t6
	li	t5, 10			# ecall from VS-mode
	beq	t6, t5, leave_guest

	addi	s2, s2, 1
	csrr	t5, mepc
	addi	t5, t5, 4
	csrw	mepc, t5
	mret

leave_guest:
	# Snapshot mstatus as the trap left it *before* touching it: MPV and
	# MPP are the evidence that the trap recorded where it came from, and
	# the mret below has to overwrite both to get back to M-mode. Reading
	# them afterwards would only show what this handler just wrote.
	csrr	s5, mstatus
	la	t5, m_after_guest
	csrw	mepc, t5
	li	t5, 3 << 11		# MPP = M
	csrs	mstatus, t5
	li	t5, 1
	slli	t5, t5, 39		# MPV = 0, or the mret re-enters the guest
	csrc	mstatus, t5
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
