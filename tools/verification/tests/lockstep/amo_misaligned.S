# Misaligned atomics, lock-stepped against Sail (BUGS 197).
#
# Every misaligned atomic is an access fault, raised from the effective
# address before translation: a store/AMO access fault for an AMO or SC, a
# load access fault for LR. That is riscv-arch-test's sail-RVA23S64 config,
# which its reference signatures come from (Spike and QEMU trap too, as
# address-misaligned); the lock-step Sail config (mkconfig.py) is set to match. The
# alternative the spec allows -- performing one that fits in a 16-byte
# misaligned atomicity granule -- is what Sail's default does, and that
# default made the two references disagree.
#
#   offsets that fit in one 16-byte block: amoadd.w +2, amoswap.d +4,
#     amoor.w +12, amoadd.h +1, amocas.w +6 (amomax.b is never misaligned)
#   offsets that cross a block: amoadd.w +14, amoswap.d +12, amoadd.h +15,
#     amocas.d +9
#   LR.W +2, LR.D +4, SC.W +2
#   under Sv39 in S-mode, with page 1 unmapped: misaligned AMOs and an LR on
#     the unmapped page -- access faults, not page faults (no translation)
# Traps go to M, which records mcause and mtval and skips the instruction.

.option arch, +zabha, +zacas

.equ PTE_RW,      0xC7          # V R W A D
.equ PTE_LEAF,    0xCF          # V R W X A D
.equ PTE_TABLE,   0x01
.equ SATP_SV39,   (8 << 60)
.equ MSTATUS_MPP, (3 << 11)
.equ MPP_S,       (1 << 11)
.equ VA,          0x40000000

.macro pte rd, rs, flags
	srli \rs, \rs, 12
	slli \rs, \rs, 10
	ori  \rd, \rs, \flags
.endm

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, -1
	csrw pmpaddr0, t0
	li   t0, 0x1f
	csrw pmpcfg0, t0
	csrw medeleg, zero
	csrw mideleg, zero

	la   s0, blk                    # 16-byte aligned, two blocks
	li   a1, 0x0101010101010101

	# ---- inside one block: performed ---------------------------------------
	amoadd.w   a2, a1, (s0)         # aligned, for reference
	addi  t0, s0, 2
	amoadd.w   a3, a1, (t0)
	addi  t0, s0, 4
	amoswap.d  a4, a1, (t0)
	addi  t0, s0, 12
	amoor.w    a5, a1, (t0)
	addi  t0, s0, 1
	amoadd.h   a6, a1, (t0)
	addi  t0, s0, 7
	amomax.b   a7, a1, (t0)
	addi  t0, s0, 6
	lw    s2, 0(t0)                 # the expected value: swaps
	mv    s3, s2
	li    s4, 0x7777
	amocas.w   s3, s4, (t0)
	ld    s5, 0(s0)
	ld    s6, 8(s0)

	# ---- crossing a block boundary: store access fault -----------------------
	addi  t0, s0, 14
	amoadd.w   a3, a1, (t0)
	addi  t0, s0, 12
	amoswap.d  a4, a1, (t0)
	addi  t0, s0, 15
	amoadd.h   a6, a1, (t0)
	addi  t0, s0, 9
	ld    s3, 0(t0)
	amocas.d   s3, a1, (t0)
	ld    s5, 8(s0)
	ld    s6, 16(s0)

	# ---- LR / SC: access fault before anything else --------------------------
	addi  t0, s0, 2
	lr.w       a3, (t0)
	addi  t0, s0, 4
	lr.d       a4, (t0)
	addi  t0, s0, 2
	sc.w       a5, a1, (t0)

	# ---- under Sv39 ------------------------------------------------------------
	la   s1, root
	li   t0, 0x80000000
	pte  t1, t0, PTE_LEAF
	sd   t1, 16(s1)                 # root[2]: 1 GB identity leaf
	la   t0, l1
	pte  t0, t0, PTE_TABLE
	sd   t0, 8(s1)                  # root[1] -> l1
	la   t1, l1
	la   t0, l0
	pte  t0, t0, PTE_TABLE
	sd   t0, 0(t1)                  # l1[0] -> l0
	la   t1, l0
	la   t0, frame
	pte  t2, t0, PTE_RW
	sd   t2, 0(t1)                  # VA page 0 -> frame; page 1 unmapped

	la   t0, root
	srli t0, t0, 12
	li   t1, SATP_SV39
	or   t0, t0, t1
	csrw satp, t0
	sfence.vma
	li   t0, MSTATUS_MPP
	csrc mstatus, t0
	li   t0, MPP_S
	csrs mstatus, t0
	la   t0, s_entry
	csrw mepc, t0
	mret

s_entry:
	li   s0, VA + 0x800
	addi t0, s0, 0x7F4
	amoadd.w   a3, a1, (t0)         # page 0, +4 in its last block: performed
	addi t0, s0, 0x7FE
	amoadd.w   a4, a1, (t0)         # crosses into page 1: access fault
	li   s1, VA + 0x1802
	amoadd.w   a5, a1, (s1)         # page 1, inside a block: page fault
	lr.w       a6, (s1)             # misaligned, unmapped: access fault
	lw   a7, 0x7F4(s0)
	ecall

done:
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	csrr t4, mtval
	li   t6, 9
	beq  t5, t6, done
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

.section .data
.align 12
root:    .fill 512, 8, 0
l1:      .fill 512, 8, 0
l0:      .fill 512, 8, 0
frame:   .fill 512, 8, 0x1111111111111111
.align 4
blk:     .dword 0x0807060504030201, 0x100F0E0D0C0B0A09
         .dword 0x1817161514131211, 0x201F1E1D1C1B1A19
.global tohost
.align 3
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
