# Svinval in U-mode (QEMU bug).
#
# SINVAL.VMA, SFENCE.W.INVAL and SFENCE.INVAL.IR are S-mode instructions:
# executed in U-mode each is an illegal-instruction exception, exactly as
# SFENCE.VMA is (privileged spec, Svinval chapter). Sail, Spike, Whisper and
# DoomV trap on all three; QEMU 10.2 traps on SINVAL.VMA but retires the two
# fences in U-mode as no-ops.
#
# Found by riscv-arch-test priv/Svinval (test2_sfence_w_inval).
#
#! qemu: trap sfence.w.inval

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
	csrw satp, zero

	li   t0, (3 << 11)
	csrc mstatus, t0               # MPP = U
	la   t0, user
	csrw mepc, t0
	mret

user:
	.word 0x16000073               # sinval.vma x0, x0
	mv   a0, t5
	.word 0x18000073               # sfence.w.inval
	mv   a1, t5
	.word 0x18100073               # sfence.inval.ir
	mv   a2, t5
	sfence.vma                     # for comparison: illegal in U too
	mv   a3, t5
	ecall

done:
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:                              # mcause in t5; skip the instruction
	csrr t5, mcause
	li   t6, 8
	beq  t5, t6, done              # the ecall from U
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
