# The machine's parameters -- what Sail's configuration sets in "memory" and
# "platform" and DoomV takes as -pmp, -pmp-grain, -asidlen, -vmidlen,
# -physaddr-bits, -cbo-block, -misaligned -- lock-stepped against Sail with
# each set otherwise (tools/verification/machine_params.py). Nothing here
# checks a value itself: each probe leaves something in a register or a
# trap in the trace, and the lock-step compares them.
#
#   PMP      pmpaddr and pmpcfg read back, at both ends of 16 and 64 entries;
#            NA4 and NAPOT under a grain; an S-mode load that the PMP allows
#            or not, by its entries' count and grain
#   satp     ASID and PPN bits kept; vsatp the ASID; hgatp VMID and PPN, and
#            a write with a mode this hart lacks, which keeps VMID and PPN
#   cbo      cbo.zero inside a block, and the bytes around it read back
#   loads    misaligned loads and stores, which run or trap
#
# Traps go to M: an ecall from S returns to M at s11; any other exception
# skips the instruction (all are 4 bytes).

.equ MSTATUS_MPP, (3 << 11)
.equ MSTATUS_MPP_S, (1 << 11)

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0

	# ---- PMP registers ------------------------------------------------------------
	li   t0, -1
	csrw pmpaddr0, t0
	csrr a0, pmpaddr0                 # physical address width
	csrw pmpaddr7, t0
	csrr a1, pmpaddr7                 # usable or not
	csrw pmpaddr15, t0
	csrr a2, pmpaddr15
	csrr a3, pmpaddr16                # exists with 64 entries only
	csrr a4, pmpaddr63
	csrr a5, pmpcfg14                 # pmpcfg14 likewise
	li   t0, 0x1018                   # entry 0 NA4 (A=2), entry 1 TOR (A=1)
	csrw pmpcfg0, t0
	csrr a6, pmpcfg0                  # NA4 is OFF with a grain
	li   t0, 0x5555
	csrw pmpaddr0, t0
	csrr a7, pmpaddr0                 # an OFF/TOR entry's low bits under a grain
	li   t0, 0x18                     # entry 0 NAPOT
	csrw pmpcfg0, t0
	csrr s2, pmpaddr0                 # a NAPOT entry's low bits under a grain
	csrw pmpcfg0, zero
	csrw pmpaddr0, zero
	csrw pmpaddr7, zero
	csrw pmpaddr15, zero

	# ---- the PMP, at work: S-mode loads ------------------------------------------------
	# Entry 0: NAPOT, the 8 bytes at pmp_word+8, no permission. Entry 1:
	# everything, RWX. S loads pmp_word+8, which entry 0 denies -- unless
	# there is no PMP at all -- and pmp_word, which only a grain of 2 or more
	# brings into entry 0's region.
	la   t0, pmp_word + 8
	srli t0, t0, 2
	csrw pmpaddr0, t0                 # NAPOT 8 bytes: no trailing ones
	li   t0, -1
	csrw pmpaddr1, t0
	li   t0, (0x1f << 8) | 0x18       # entry 0 NAPOT, no permission; entry 1 NAPOT RWX
	csrw pmpcfg0, t0
	la   s11, after_s
	li   t0, MSTATUS_MPP
	csrc mstatus, t0
	li   t0, MSTATUS_MPP_S
	csrs mstatus, t0
	la   t0, in_s
	csrw mepc, t0
	mret
in_s:
	la   t1, pmp_word
	ld   s3, 0(t1)                    # allowed, unless a grain widens entry 0
	ld   s4, 8(t1)                    # denied, unless there is no PMP
	ecall
after_s:
	csrw pmpcfg0, zero
	csrw pmpaddr0, zero
	csrw pmpaddr1, zero

	# ---- satp, vsatp, hgatp ---------------------------------------------------------
	li   t0, (8 << 60) | (0xFFFF << 44) | 0xFFFFFFFFFFF
	csrw satp, t0
	csrr s5, satp                     # ASID and PPN bits kept
	csrw satp, zero
	csrw vsatp, t0
	csrr s6, vsatp                    # ASID bits kept; the PPN whole
	csrw vsatp, zero
	li   t0, (8 << 60) | (0x3FFF << 44) | 0xFFFFFFFFFFF
	csrw hgatp, t0
	csrr s7, hgatp                    # VMID and PPN bits kept
	li   t0, (5 << 60) | (0x1234 << 44) | 0x0ABCDE
	csrw hgatp, t0
	csrr s8, hgatp                    # a mode it lacks: MODE kept, VMID and PPN taken
	csrw hgatp, zero

	# ---- cbo.zero ---------------------------------------------------------------------
	la   s0, cbo_area
	li   t0, -1
	li   t1, 0
1:	add  t2, s0, t1
	sd   t0, 0(t2)
	addi t1, t1, 8
	li   t2, 512
	blt  t1, t2, 1b
	addi t0, s0, 136                  # inside the block from 128 (or 0 or 64)
	cbo.zero (t0)
	ld   a0, 0(s0)
	ld   a1, 56(s0)
	ld   a2, 64(s0)
	ld   a3, 120(s0)
	ld   a4, 128(s0)
	ld   a5, 152(s0)
	ld   a6, 160(s0)
	ld   a7, 248(s0)
	ld   s2, 256(s0)

	# ---- misaligned loads and stores ---------------------------------------------------
	la   s0, cbo_area
	li   t0, 0x0102030405060708
	sd   t0, 0(s0)
	lw   s3, 1(s0)
	lh   s4, 3(s0)
	ld   s5, 4(s0)
	sw   t0, 6(s0)
	sh   t0, 9(s0)
	ld   s6, 0(s0)
	ld   s7, 8(s0)

	# ---- done ----------------------------------------------------------------------
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	li   t6, 9                        # ecall from S
	beq  t5, t6, from_s
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret
from_s:
	li   t6, MSTATUS_MPP
	csrs mstatus, t6
	csrw mepc, s11
	mret

.section .data
.align 12
cbo_area: .fill 512, 1, 0
.align 6
pmp_word: .dword 0x1111111111111111, 0x2222222222222222
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
