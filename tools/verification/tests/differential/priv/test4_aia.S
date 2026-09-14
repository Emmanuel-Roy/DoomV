# Hand-written verification for Stage 2's AIA path (IMSIC + APLIC), same
# breakpoint/crash.log methodology as the other other priv test files. No real
# wired device exists in this emulator, so the "device" here is this
# test writing directly to APLIC's setipnum -- exactly the mechanism the
# plan called for.
#
# M-mode enables mie.SEIE + mstatus.MIE (mideleg is left at 0, so this
# S-external-caused interrupt is taken directly by M, per spec -- M
# always sees mie's bits regardless of delegation, delegation only
# changes which handler an *S/U-mode-visible* trap would run), then
# configures the S-file IMSIC (siselect/sireg: eidelivery=1, ID 5
# enabled) and APLIC (domaincfg.IE, source 1 active, target[1].EIID=5),
# then fires source 1 via setipnum -- which should immediately raise
# SEIP and be taken as cause (1<<63)|9. The handler peeks stopei (a
# plain csrr, no side effect), then claims it with an actual write
# (csrrw), and confirms a second peek reads back 0.
.section .text.start
.global _start

.equ MIE_SEIE,    (1 << 9)
.equ MSTATUS_MIE, (1 << 3)
.equ IMSIC_SEL_EIDELIVERY, 0x70
.equ IMSIC_SEL_EIE0,       0xC0
.equ APLIC_DOMAINCFG,  0x0C000000
.equ APLIC_SOURCECFG1, 0x0C000004
.equ APLIC_TARGET1,    0x0C003004
.equ APLIC_SETIPNUM,   0x0C001CDC
.equ DOMAINCFG_IE,     (1 << 8)
.equ EIID,             5

_start:
	la   t0, m_trap
	csrw mtvec, t0

	li   t0, MIE_SEIE
	csrw mie, t0
	li   t0, MSTATUS_MIE
	csrs mstatus, t0

	li   t0, IMSIC_SEL_EIDELIVERY
	csrw siselect, t0
	li   t0, 1
	csrw sireg, t0

	li   t0, IMSIC_SEL_EIE0
	csrw siselect, t0
	li   t0, (1 << EIID)
	csrw sireg, t0

	li   t0, APLIC_DOMAINCFG
	li   t1, DOMAINCFG_IE
	sw   t1, 0(t0)

	li   t0, APLIC_SOURCECFG1
	li   t1, 1
	sw   t1, 0(t0)

	li   t0, APLIC_TARGET1
	li   t1, EIID
	sw   t1, 0(t0)

	li   t0, APLIC_SETIPNUM
	li   t1, 1              # source number 1
	sw   t1, 0(t0)           # should immediately raise SEIP via the S-file IMSIC

spin:
	addi s0, s0, 1
	j    spin

	.align 2
m_trap:
	csrr a0, mcause          # expect (1<<63)|9
	csrr a1, stopei          # peek, no side effect -- expect (EIID<<16)|EIID
	csrrw a2, stopei, zero   # claim -- written value is ignored
	csrr a3, stopei          # expect 0 now that it's claimed
	j    halt

halt:
	j    halt
