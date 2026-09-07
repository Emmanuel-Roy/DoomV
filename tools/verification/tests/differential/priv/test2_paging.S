# Hand-written verification for Stage 1's Sv39 MMU. Same methodology as
# test1_delegation.S: run under DoomV with a breakpoint at `halt`, read
# a0/a1/a2 back out of crash.log.
#
# M-mode delegates page faults to S and MRETs into S-mode code that builds
# a single-entry Sv39 root table mapping [0x80000000, 0xC0000000) identity
# via one 1GB superpage (V|R|W|X|A|D, ppn=RAM_BASE>>12 -- covers this
# program's own code, so execution keeps running seamlessly once satp goes
# live), enables paging, then: (Test A) stores/loads a scratch word through
# the now-mapped region and expects it to just work, and (Test B) loads
# from an address whose VPN[2] index was never given a valid entry,
# expecting a load page fault (cause 13) with stval equal to the faulting VA.
.section .text.start
.global _start

# ROOT_TABLE = RAM_BASE + 0x2000 -- must be a real RAM address (page tables
# are walked via ordinary physical loads/stores), just past this tiny
# ELF's own code+data. RAM_BASE moved to 0x80000000 for Stage 3, which
# also moved which gigapage (VPN[2] index) covers it -- see the table[0]
# comment below.
.equ ROOT_TABLE,   0x80002000
.equ UNMAPPED_VA,  0x40000000   # VPN[2]=1 -- deliberately left invalid

_start:
	la   t0, m_trap
	csrw mtvec, t0

	li   t0, (1<<12)|(1<<13)|(1<<15)  # delegate instr/load/store page faults to S
	csrw medeleg, t0

	csrr t1, mstatus
	li   t2, ~0x1800
	and  t1, t1, t2
	li   t2, (1 << 11)                # MPP = S
	or   t1, t1, t2
	csrw mstatus, t1

	la   t0, s_entry
	csrw mepc, t0
	mret

	.align 2
s_entry:
	la   t0, s_trap
	csrw stvec, t0

	# Zero the root table (512 x 8-byte entries) -- satp is still 0 (bare)
	# at this point, so these are plain physical addresses.
	li   t0, ROOT_TABLE
	li   t1, 512
1:
	sd   zero, 0(t0)
	addi t0, t0, 8
	addi t1, t1, -1
	bnez t1, 1b

	# table[2]: VPN[2]=2 covers VA [0x80000000, 0xC0000000) -- RAM_BASE's
	# own gigapage, since RAM_BASE=0x80000000 (see comment above). A 1GB
	# superpage leaf identity-mapping it: V|R|W|X|A|D set, ppn=0x80000000>>12
	# (see mmu.cpp: PA = (ppn<<12) | (va & mask), so ppn==VA>>12 over this
	# range means PA==VA within it).
	li   t0, ROOT_TABLE
	li   t2, 0xCF
	li   t3, 0x80000000 >> 12
	slli t3, t3, 10        # PTE ppn field starts at bit 10
	or   t2, t2, t3
	sd   t2, 16(t0)

	li   t0, ROOT_TABLE
	srli t0, t0, 12   # satp.PPN = table's physical page number
	li   t2, 8
	slli t2, t2, 60   # satp.MODE = 8 (Sv39)
	or   t0, t0, t2
	csrw satp, t0
	sfence.vma

	# ---- Test A: mapped access should just work ----
	la   t0, scratch
	li   t1, 0x1234abcd
	sd   t1, 0(t0)
	ld   a2, 0(t0)     # expect 0x1234abcd back if translation round-trips correctly

	# ---- Test B: unmapped access should page-fault ----
	li   t0, UNMAPPED_VA
	ld   t1, 0(t0)     # expected to trap into s_trap, not complete
	li   a2, 0xbaadf00d # must NOT execute
	j    halt

	.align 2
s_trap:
	csrr a0, scause    # expect 13 (load page fault)
	csrr a1, stval     # expect UNMAPPED_VA
	j    halt

	.align 2
m_trap:
	li   a0, 0xf00dbaad # should not be reached -- would mean delegation failed
	j    halt

halt:
	j    halt

.section .data
.align 3
scratch:
	.dword 0
