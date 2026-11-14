# What minstret and mcycle read (platform and implementation choices;
# recorded, not bugs -- except that QEMU's minstret is not a count).
#
# The program retires 201 instructions and reads minstret, then mcycle.
#
#   minstret  Sail, DoomV, Whisper: 201. Spike: 206 -- it also counts the
#             five instructions of its boot ROM at 0x1000, which run before
#             the program's entry. QEMU (without -icount): the host's clock,
#             as for mcycle; it does not count instructions at all.
#   mcycle    implementation-defined: Sail's (and so DoomV's) advances on
#             its platform clock, Whisper's once per instruction, so the two
#             part here.
#
# Spike and QEMU leave Sail at the first read, Whisper at the second.
#
# Found by riscv-arch-test Zicntr-csrrs-00 / csrrc-00.
#
#! spike: x10 csrr
#! whisper: x11 csrr
#! qemu: x10 csrr

.section .text.init
.global _start
_start:
	li   t0, 100
1:	addi t0, t0, -1
	bnez t0, 1b
	csrr a0, minstret
	csrr a1, mcycle

	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
2:	j    2b

.section .data
.align 3
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
