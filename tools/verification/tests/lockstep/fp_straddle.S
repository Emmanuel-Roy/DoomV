# FP loads and stores that straddle a page, lock-stepped. They used to
# translate only their first byte: a straddle into an unmapped or read-only
# page went ahead, reading or writing whatever followed the first page
# *physically*, and PMP saw one byte of it. Every register write, trap and
# store is compared against Sail's trace, so each case below shows up as a
# value, a missing fault or a stray store.
#
#   FLD / FLW / FLH straddling two mapped pages that are not physically
#     adjacent -- the second page's bytes must come from its own frame
#   FSD straddling them, read back as integers from both frames
#   FSD straddling into a read-only page -- a store page fault naming the
#     boundary, after the part in the first page is written (Sail's split)
#   FLD straddling into that read-only page -- allowed, both frames read
#   FLD straddling into an unmapped page -- a load page fault at the boundary
#   the same straddles from integer loads and stores, which share the path
#   SD straddling with paging off, in M-mode -- one access, not split
#
# Sv39, S-mode, mstatus.FS on. 0x80000000 is identity-mapped by a 1 GB leaf;
# 0x40000000 holds four 4 KB pages:
#   0 -> frame_a      1 -> frame_b (a lower frame: not the next one)
#   2 -> frame_c, read-only      3 -> unmapped
# Traps go to M, which records mcause and mtval and skips the instruction.

.option arch, +f, +d, +zfhmin

.equ PTE_RW,      0xC7          # V R W A D
.equ PTE_RO,      0x43          # V R A
.equ PTE_LEAF,    0xCF          # V R W X A D
.equ PTE_TABLE,   0x01
.equ SATP_SV39,   (8 << 60)
.equ MSTATUS_MPP, (3 << 11)
.equ MPP_S,       (1 << 11)
.equ MSTATUS_FS,  (1 << 13)     # Initial
.equ VA,          0x40000000

# rd = a PTE with `flags` for the page at the address in rs (clobbers rs)
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

	la   s0, root
	li   t0, 0x80000000
	pte  t1, t0, PTE_LEAF
	sd   t1, 16(s0)                # root[2]: 1 GB identity leaf
	la   t0, l1
	pte  t0, t0, PTE_TABLE
	sd   t0, 8(s0)                 # root[1] -> l1
	la   t1, l1
	la   t0, l0
	pte  t0, t0, PTE_TABLE
	sd   t0, 0(t1)                 # l1[0] -> l0
	la   t1, l0
	la   t0, frame_a
	pte  t2, t0, PTE_RW
	sd   t2, 0(t1)
	la   t0, frame_b
	pte  t2, t0, PTE_RW
	sd   t2, 8(t1)
	la   t0, frame_c
	pte  t2, t0, PTE_RO
	sd   t2, 16(t1)

	# Paging off (M-mode, satp Bare): a straddle is one access, not split.
	li   t0, 0x0102030405060708
	la   t1, frame_a + 0xFFD
	sd   t0, 0(t1)
	ld   a0, 0(t1)

	la   t0, root
	srli t0, t0, 12
	li   t1, SATP_SV39
	or   t0, t0, t1
	csrw satp, t0
	sfence.vma

	li   t0, MSTATUS_MPP
	csrc mstatus, t0
	li   t0, MPP_S | MSTATUS_FS
	csrs mstatus, t0
	la   t0, s_entry
	csrw mepc, t0
	mret

s_entry:
	# Bases mid-page, so that every offset below fits in 12 bits.
	li   s0, VA + 0x800
	li   s1, VA + 0x1800
	li   s2, VA + 0x2800

	# ---- across pages 0 and 1, both mapped, frames not adjacent ---------------
	fld   f1, 0x7FC(s0)
	fmv.x.d a0, f1
	flw   f2, 0x7FE(s0)
	fmv.x.w a1, f2
	flh   f3, 0x7FF(s0)
	fmv.x.d a2, f3

	li    t0, 0x1122334455667788
	fmv.d.x f4, t0
	fsd   f4, 0x7FA(s0)
	la    t1, frame_a + 0xFF8
	ld    a3, 0(t1)
	la    t1, frame_b
	ld    a4, 0(t1)

	# ---- from page 1 into page 2, read-only ------------------------------------
	fld   f5, 0x7FC(s1)            # a load may straddle into it
	fmv.x.d a6, f5
	li    t0, 0x99AABBCCDDEEFF00
	fmv.d.x f6, t0
	fsd   f6, 0x7FC(s1)            # store page fault at VA + 0x2000
	# The part in page 1 is written before the fault, as Sail splits it. The
	# architecture allows either (a misaligned access may be split, and trap
	# part-way); this is the choice Sail makes, so it is the one DoomV makes.
	la    t1, frame_b + 0xFF8
	ld    a5, 0(t1)
	fld   f9, 0x7FC(s1)
	fmv.x.d s5, f9

	# ---- from page 2 into page 3, unmapped -------------------------------------
	fld   f7, 0x7FC(s2)            # load page fault at VA + 0x3000
	flw   f8, -0x800(s2)                # inside page 2: fine
	fmv.x.w a7, f8

	# ---- the same, from integer code: one path serves both --------------------
	li    t0, 0x0A0B0C0D0E0F1011
	sd    t0, 0x7FD(s1)            # store page fault; page 1's 3 bytes written
	la    t1, frame_b + 0xFF8
	ld    s6, 0(t1)
	lw    s7, 0x7FE(s0)            # straddling load across pages 0 and 1
	lhu   s8, 0x7FF(s1)            # into the read-only page: allowed

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
# frame_b first, so that frame_a's next frame is frame_c and not frame_b: a
# straddle read physically would get frame_c's bytes.
frame_b:
	.dword 0x0F0E0D0C0B0A0908, 0x1716151413121110
	.fill 510, 8, 0x2121212121212121
frame_a:
	.fill 511, 8, 0x3131313131313131
	.dword 0x4746454443424140
frame_c:
	.dword 0x5756555453525150
	.fill 511, 8, 0x6161616161616161
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
