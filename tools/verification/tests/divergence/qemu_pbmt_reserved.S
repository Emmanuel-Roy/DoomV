# A leaf PTE with the reserved PBMT encoding 3 (QEMU bug).
#
# With Svpbmt (menvcfg.PBMTE = 1), PTE bits 62:61 select the memory type:
# 0 PMA, 1 NC, 2 IO, and 3 is reserved -- a leaf with PBMT = 3 is treated
# as invalid, so an access through it is a page fault (privileged spec,
# Svpbmt; Sv39 walk step 3). Sail, Spike, Whisper and DoomV fault; QEMU 10.2
# performs the access.
#
#   page 0: PBMT = 1 (NC), a normal mapping -- the store and load work
#   page 1: PBMT = 3 -- the store is a store page fault (15), the load a
#           load page fault (13), in S-mode with the traps going to M
#
# Found by riscv-arch-test priv/sv39_Svpbmt_Smode.
#
#! qemu: trap sw

.equ PTE_RW,    0xC7                # V R W A D
.equ PTE_LEAF,  0xCF                # V R W X A D
.equ PTE_TABLE, 0x01
.equ VA,        0x40000000

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
	li   t0, 1
	slli t0, t0, 62
	csrs menvcfg, t0               # PBMTE

	la   s1, root
	li   t0, 0x80000000
	pte  t1, t0, PTE_LEAF
	sd   t1, 16(s1)                # root[2]: 1 GB identity leaf (the code)
	la   t0, l1
	pte  t0, t0, PTE_TABLE
	sd   t0, 8(s1)                 # root[1] -> l1
	la   t1, l1
	la   t0, l0
	pte  t0, t0, PTE_TABLE
	sd   t0, 0(t1)                 # l1[0] -> l0
	la   t1, l0
	la   t0, frame0
	pte  t2, t0, PTE_RW
	li   t3, 1
	slli t3, t3, 61
	or   t2, t2, t3                # PBMT = 1
	sd   t2, 0(t1)                 # VA page 0 -> frame0, NC
	la   t0, frame1
	pte  t2, t0, PTE_RW
	li   t3, 3
	slli t3, t3, 61
	or   t2, t2, t3                # PBMT = 3, reserved
	sd   t2, 8(t1)                 # VA page 1 -> frame1

	la   t0, root
	srli t0, t0, 12
	li   t1, (8 << 60)
	or   t0, t0, t1
	csrw satp, t0
	sfence.vma
	li   t0, (3 << 11)
	csrc mstatus, t0
	li   t0, (1 << 11)
	csrs mstatus, t0               # MPP = S
	la   t0, super
	csrw mepc, t0
	mret

super:
	li   s0, VA
	li   a1, 0x1234
	sw   a1, 0(s0)                 # NC: retires
	lw   a2, 0(s0)
	li   s1, VA + 0x1000
	li   t5, 0
	sw   a1, 0(s1)                 # PBMT = 3: store page fault
	mv   a3, t5
	li   t5, 0
	lw   a4, 0(s1)                 # load page fault
	mv   a5, t5
	ecall

done:
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	li   t6, 9
	beq  t5, t6, done              # the ecall from S
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

.section .data
.align 12
root:    .fill 512, 8, 0
l1:      .fill 512, 8, 0
l0:      .fill 512, 8, 0
frame0:  .fill 512, 8, 0
frame1:  .fill 512, 8, 0
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
