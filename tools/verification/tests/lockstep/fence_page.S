# Address fences, lock-stepped. SFENCE.VMA and SINVAL.VMA with rs1 != x0
# drop only the page they name (RiscvCore::fence_page), so this remaps pages
# the TLB, the fetch cache and the data caches have all just used, fences
# each one by address, and uses it again. Every register write is compared
# against Sail's trace, so a stale translation shows up as the old value.
#
#   a 4 KB data page, loaded twice (the second a cache hit), remapped
#   a 4 KB code page, called twice, remapped
#   a 4 KB page stored to twice, remapped, fenced with an ASID, stored to again
#   a 2 MB superpage, remapped and fenced at a *different* 4 KB page inside
#     it: the fence covers the whole leaf, so the page used must drop too
#   the same with SINVAL.VMA between SFENCE.W.INVAL and SFENCE.INVAL.IR
#
# Only fences the architecture requires to take effect are tested. Whether a
# remap *without* a matching fence is seen is the implementation's choice,
# and diffing it would diff an open choice.
#
# Sv39, S-mode. 0x80000000 is identity-mapped by a 1 GB leaf (code, data and
# the page tables); 0x40000000 holds the 4 KB pages; 0xC0000000 the 2 MB one.

.equ PTE_LEAF,    0xCF          # V R W X A D: no A/D update is ever needed
.equ PTE_TABLE,   0x01
.equ SATP_SV39,   (8 << 60)
.equ MSTATUS_MPP, (3 << 11)
.equ MPP_S,       (1 << 11)
.equ VA_DATA,     0x40000000
.equ VA_CODE,     0x40001000
.equ VA_STORE,    0x40002000
.equ VA_SUPER,    0xC0000000
.equ PA_SUPER1,   0x80200000
.equ PA_SUPER2,   0x80400000

# rd = the leaf PTE for the page at the address in rs (clobbers rs)
.macro leaf rd, rs
	srli \rs, \rs, 12
	slli \rs, \rs, 10
	ori  \rd, \rs, PTE_LEAF
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

	# The two halves of the superpage's target, told apart by value.
	li   t0, PA_SUPER1 + 0x1000
	li   t1, 0x5151
	sd   t1, 0(t0)
	li   t0, PA_SUPER2 + 0x1000
	li   t1, 0x5252
	sd   t1, 0(t0)

	# root[2]: 1 GB identity leaf for 0x80000000
	la   s0, root
	li   t0, 0x80000000
	leaf t1, t0
	sd   t1, 16(s0)
	# root[1] -> l1_lo -> l0 for 0x40000000
	la   t0, l1_lo
	srli t0, t0, 12
	slli t0, t0, 10
	ori  t0, t0, PTE_TABLE
	sd   t0, 8(s0)
	la   t1, l1_lo
	la   t0, l0
	srli t0, t0, 12
	slli t0, t0, 10
	ori  t0, t0, PTE_TABLE
	sd   t0, 0(t1)
	# root[3] -> l1_hi, whose entry 0 is a 2 MB leaf for 0xC0000000
	la   t0, l1_hi
	srli t0, t0, 12
	slli t0, t0, 10
	ori  t0, t0, PTE_TABLE
	sd   t0, 24(s0)
	la   t1, l1_hi
	li   t0, PA_SUPER1
	leaf t2, t0
	sd   t2, 0(t1)
	# l0[0] data, l0[1] code, l0[2] store
	la   t1, l0
	la   t0, data1
	leaf t2, t0
	sd   t2, 0(t1)
	la   t0, code1
	leaf t2, t0
	sd   t2, 8(t1)
	la   t0, store1
	leaf t2, t0
	sd   t2, 16(t1)

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
	# ---- a data page -----------------------------------------------------------
	li   s0, VA_DATA
	ld   a0, 0(s0)
	ld   a0, 0(s0)
	la   t0, data2
	leaf t2, t0
	la   t1, l0
	sd   t2, 0(t1)
	sfence.vma s0
	ld   a1, 0(s0)
	ld   a1, 0(s0)

	# ---- a code page -----------------------------------------------------------
	li   s1, VA_CODE
	jalr s1
	jalr s1
	la   t0, code2
	leaf t2, t0
	la   t1, l0
	sd   t2, 8(t1)
	sfence.vma s1
	jalr s1
	jalr s1

	# ---- a page stored to ------------------------------------------------------
	li   s2, VA_STORE
	li   t0, 0x77
	sd   t0, 0(s2)
	sd   t0, 8(s2)
	la   t0, store2
	leaf t2, t0
	la   t1, l0
	sd   t2, 16(t1)
	# With an ASID operand. It has to be satp's (0): a fence naming another
	# ASID need not touch this page, and Sail does not.
	li   t3, 0
	sfence.vma s2, t3
	li   t0, 0x88
	sd   t0, 0(s2)
	ld   a2, 0(s2)
	la   t0, store1
	ld   a3, 0(t0)
	ld   a4, 8(t0)
	la   t0, store2
	ld   a5, 0(t0)

	# ---- a superpage, fenced elsewhere inside it -------------------------------
	li   s3, VA_SUPER + 0x1000
	ld   a0, 0(s3)
	ld   a0, 0(s3)
	li   t0, PA_SUPER2
	leaf t2, t0
	la   t1, l1_hi
	sd   t2, 0(t1)
	li   t0, VA_SUPER + 0x5000
	sfence.vma t0
	ld   a1, 0(s3)

	# ---- SINVAL.VMA, back to the first data page -------------------------------
	ld   a0, 0(s0)
	la   t0, data1
	leaf t2, t0
	la   t1, l0
	sd   t2, 0(t1)
	.insn r 0x73, 0, 0x0c, x0, x0, x0     # sfence.w.inval
	.insn r 0x73, 0, 0x0b, x0, s0, x0     # sinval.vma s0
	.insn r 0x73, 0, 0x0c, x0, x0, x1     # sfence.inval.ir
	ld   a1, 0(s0)

	ecall

	# ---- done ----------------------------------------------------------------------
done:
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	li   t6, 9
	beq  t5, t6, done
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

	.align 12
code1:
	li   a0, 0x11
	ret
	.align 12
code2:
	li   a0, 0x22
	ret

.section .data
.align 12
root:   .fill 512, 8, 0
l1_lo:  .fill 512, 8, 0
l1_hi:  .fill 512, 8, 0
l0:     .fill 512, 8, 0
data1:  .dword 0x1111
.align 12
data2:  .dword 0x2222
.align 12
store1: .fill 512, 8, 0
store2: .fill 512, 8, 0
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
