# The cause a misaligned AMO raises (QEMU bug).
#
# An AMO or SC is a store for exception purposes: misaligned, it raises
# either store/AMO address-misaligned (6) or store/AMO access-fault (7).
# A misaligned LR may raise load address-misaligned (4) or load
# access-fault (5). The two Sail configs pick the access faults (BUGS 197);
# Spike (with Zicclsm), Whisper (misal_acc_fault) and DoomV do the same.
#
# QEMU 10.2 raises 4 -- load address-misaligned -- for every one of them,
# AMOs and SC included, which no choice the spec allows gives for a store.
# (Its 4 for LR would be allowed; the AMOs are the bug.)
#
# Found by riscv-arch-test sv39_exceptions_Zaamo_* and _Zalrsc_*.
#
#! qemu: x30 csrr

.option arch, +zaamo, +zalrsc

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, -1
	csrw pmpaddr0, t0
	li   t0, 0x1f
	csrw pmpcfg0, t0

	la   s0, blk
	li   a7, 1
	addi s1, s0, 2
	amoadd.w  t1, a7, (s1)         # 7
	mv   a0, t5
	addi s1, s0, 4
	amoswap.d t1, a7, (s1)         # 7
	mv   a1, t5
	addi s1, s0, 2
	sc.w t1, a7, (s1)              # 7
	mv   a2, t5
	lr.w t1, (s1)                  # 5
	mv   a3, t5

	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
	csrr t5, mcause
	csrr t6, mepc
	addi t6, t6, 4
	csrw mepc, t6
	mret

.section .data
.align 4
blk:      .dword 0, 0
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
