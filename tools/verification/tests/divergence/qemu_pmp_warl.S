# What pmpaddr and pmpcfg hold after all-ones and reserved-bit writes
# (QEMU bugs).
#
# pmpaddr: on RV64 it holds bits 55:2 of a physical address, and with
# 56-bit physical addresses (the config's physaddr_bits) bits 63:54 are
# WARL zero: writing all ones reads back 0x003fffffffffffff. Sail, Spike,
# Whisper and DoomV do; QEMU 10.2 reads back all 64 ones.
#
# pmpcfg: bits 6:5 of each entry's byte are reserved, WARL zero. Writing
# 0x20 into a byte reads back 0. QEMU keeps the bit. (The test writes it to
# entry 1, with A = 0 so no region changes.)
#
# Found by riscv-arch-test PMPSm_grain and PMPSm_pmpcfg_walk_02..09.
#
#! qemu: x10 csrr

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0

	li   t0, -1
	csrw pmpaddr1, t0
	csrr a0, pmpaddr1              # 0x003fffffffffffff
	csrw pmpaddr1, zero

	li   t0, 0x2000
	csrw pmpcfg0, t0               # entry 1: reserved bit 5
	csrr a1, pmpcfg0               # 0
	li   t0, 0x4000
	csrw pmpcfg0, t0               # entry 1: reserved bit 6
	csrr a2, pmpcfg0               # 0

	li   t0, -1
	csrw pmpaddr0, t0
	li   t0, 0x1f
	csrw pmpcfg0, t0

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
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
