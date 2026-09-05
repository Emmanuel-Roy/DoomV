# Differential vector test: exercises the V-extension operations a real
# riscv64 userspace actually executes (the ~50 distinct ops a distro-built
# static glibc emits, harvested by objdump over BusyBox), and dumps every
# result into a signature region for byte-exact comparison against spike.
#
# The V extension has no upstream riscv-arch-test suite, so this is the
# substitute: same signature-region contract the arch-test harness already
# uses (begin_signature/end_signature), so both simulators can be diffed
# with the existing tooling rather than a bespoke comparison.
#
# Halting works for both targets from one binary: the HTIF tohost write
# terminates spike, and the spin loop right after it is where DoomV is
# told to break (it has no HTIF -- see tools/vtest/doomv_env/model_test.h).
#
# VLEN is pinned to 128 on both sides (DoomV's Registers::VLEN_BITS, and
# spike via --varch=vlen:128,elen:64). Results are only comparable if the
# two agree, since vl and every VLMAX-dependent result changes with it.
#
# Not covered yet, though BusyBox does use them -- each needs more setup
# than a straight-line dump (or an extension beyond base V) and is worth
# its own test once the basics are known good:
#   vle8ff.v      fault-only-first, whose whole point is trimming vl on a
#                 page fault, so it needs a deliberately faulting page
#   vluxei64.v    indexed load, needs an index vector and a mapped spread
#   vsseg4e8.v    segment store, 4-field structure interleaving
#   vrgatherei16.vv  gather with a 16-bit index vector (mixed EEW)
#   vandn.vv      Zvbb, not base V
#   vzext.vf8     needs LMUL=8 to be a legal encoding (see below)

.section .text.start
.global _start
_start:
	# Enable the vector unit before touching it: mstatus.VS (bits 10:9) is
	# Off at reset, and a real implementation traps every vector instruction
	# as illegal until software sets it. Writing Initial (01) is enough.
	# Spike enforces this; DoomV currently does not, which is itself worth
	# knowing -- without this the test traps on the first vsetivli under the
	# reference model while running fine under the DUT.
	li	t0, 0x200
	csrs	mstatus, t0
	la	sp, __stacktop
	la	s0, begin_signature	# s0 = rolling signature write pointer
	la	s1, testdata_a
	la	s2, testdata_b

# Store the full 128-bit vector register v(N) into the signature area and
# advance the pointer. Uses e8/m1 so the store is width-agnostic: always
# exactly VLEN/8 = 16 bytes regardless of the vtype the test just used.
# vtype/vl are saved and restored around the dump. Without that, this
# macro's own vsetvli silently becomes the vtype for whatever instruction
# follows -- which made vzext.vf2 run at SEW=8, where its source EEW of 4
# is below the 8-bit minimum and the encoding is illegal. Every op after
# the first SIGV would otherwise have been tested at e8/m1 rather than the
# width its surrounding comment claims.
.macro SIGV vreg
	csrr	t4, vtype
	csrr	t5, vl
	vsetvli	t6, x0, e8, m1, ta, ma
	vse8.v	\vreg, (s0)
	addi	s0, s0, 16
	vsetvl	t6, t5, t4
.endm

# Store a scalar (an x-register result, e.g. a vl or a vmv.x.s extract).
.macro SIGX xreg
	sd	\xreg, 0(s0)
	addi	s0, s0, 8
.endm

	#---- vset{i}vli: vl computation is the foundation everything else
	#---- depends on, and the exact bug class that broke glibc's memset.
	vsetivli t0, 2, e64, m1, ta, ma
	SIGX	t0
	vsetivli t0, 31, e8, m1, ta, ma		# AVL > VLMAX must clamp to VLMAX
	SIGX	t0
	li	t1, 100
	vsetvli	t0, t1, e8, m1, ta, ma		# ditto via the register form
	SIGX	t0
	li	t1, 3
	vsetvli	t0, t1, e32, m1, ta, ma		# AVL < VLMAX passes through
	SIGX	t0
	vsetvli	t0, x0, e32, m1, ta, ma		# rs1=x0 means "give me VLMAX"
	SIGX	t0

	#---- loads and stores at every element width
	vsetvli	t0, x0, e8, m1, ta, ma
	vle8.v	v1, (s1)
	SIGV	v1
	vsetvli	t0, x0, e16, m1, ta, ma
	vle16.v	v2, (s1)
	SIGV	v2
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v3, (s1)
	SIGV	v3
	vsetvli	t0, x0, e64, m1, ta, ma
	vle64.v	v4, (s1)
	SIGV	v4

	#---- integer arithmetic: vv / vx / vi operand forms
	vsetvli	t0, x0, e64, m1, ta, ma
	vle64.v	v1, (s1)
	vle64.v	v2, (s2)
	vadd.vv	v5, v1, v2
	SIGV	v5
	li	t1, 0x1234
	vadd.vx	v5, v1, t1
	SIGV	v5
	vadd.vi	v5, v1, 7
	SIGV	v5
	vrsub.vx v5, v1, t1
	SIGV	v5
	vrsub.vi v5, v1, 5
	SIGV	v5
	vmul.vx	v5, v1, t1
	SIGV	v5

	#---- bitwise
	vand.vv	v5, v1, v2
	SIGV	v5
	vand.vi	v5, v1, 12
	SIGV	v5
	vor.vv	v5, v1, v2
	SIGV	v5
	vnot.v	v5, v1
	SIGV	v5

	#---- shifts
	vsll.vi	v5, v1, 3
	SIGV	v5
	li	t1, 5
	vsll.vx	v5, v1, t1
	SIGV	v5
	vsrl.vi	v5, v1, 2
	SIGV	v5
	vsrl.vx	v5, v1, t1
	SIGV	v5

	#---- moves, splats and scalar extraction
	vmv.v.i	v5, 9
	SIGV	v5
	li	t1, 0x55aa
	vmv.v.x	v5, t1
	SIGV	v5
	vmv1r.v	v6, v1
	SIGV	v6
	vmv.x.s	t2, v1			# element 0 out to a scalar
	SIGX	t2
	li	t1, 0x77
	vmv.s.x	v7, t1			# scalar into element 0 only
	SIGV	v7

	#---- vid: element indices, the basis of most gather/stride idioms
	vid.v	v8
	SIGV	v8

	#---- mask-producing compares, then mask consumers
	vmseq.vi v0, v1, 0
	SIGV	v0
	vmsne.vv v9, v1, v2
	SIGV	v9
	vmor.mm	v10, v0, v9
	SIGV	v10
	vfirst.m t3, v9			# index of first set mask bit, -1 if none
	SIGX	t3

	#---- merge under a mask (v0 is the implicit mask register)
	vmseq.vi v0, v8, 0
	vmerge.vim v11, v1, 3, v0
	SIGV	v11
	vmerge.vvm v11, v1, v2, v0
	SIGV	v11

	#---- permutes: gather and slide
	vid.v	v12
	vrgather.vv v13, v1, v12
	SIGV	v13
	vslidedown.vi v14, v1, 1
	SIGV	v14

	#---- reductions (scalar result lands in element 0 of the dest)
	vmv.s.x	v15, x0
	vredsum.vs v16, v1, v15
	SIGV	v16
	vmv.s.x	v15, x0
	vredor.vs v17, v1, v15
	SIGV	v17

	#---- widening and narrowing, where element-width bookkeeping is
	#---- easiest to get wrong
	# Register numbering matters here in a way it does not elsewhere: a
	# widening op's wide operand (and destination) is a 2-register group,
	# so those operands must be even-numbered. vwaddu.wv takes vs2 as the
	# already-wide side -- passing an odd register there is an illegal
	# encoding, which spike rejects outright.
	vsetvli	t0, x0, e32, m1, ta, ma
	vle32.v	v2, (s1)
	vle32.v	v4, (s2)
	vwaddu.vv v18, v2, v4		# both narrow -> wide dest group (v18,v19)
	SIGV	v18
	vwaddu.wv v22, v18, v4		# wide vs2 (even) + narrow vs1
	SIGV	v22
	vnsrl.wi v20, v18, 4		# wide vs2 (even) -> narrow dest
	SIGV	v20

	#---- integer extension by a fixed factor
	vsetvli	t0, x0, e64, m1, ta, ma
	vsext.vf4 v20, v1
	SIGV	v20
	# vf8 at LMUL=1 would give the source an EMUL of 1/8 -- the absolute
	# minimum the spec allows, and spike rejects this encoding outright.
	# Covering it properly needs LMUL=8 so the source lands at EMUL=1,
	# which means dumping an 8-register group; left for its own test.
	vzext.vf2 v21, v1
	SIGV	v21

	#---- whole-register and mask-granular load/store forms
	vl1r.v	v22, (s1)
	SIGV	v22
	vs1r.v	v22, (s0)
	addi	s0, s0, 16
	vsetvli	t0, x0, e8, m1, ta, ma
	vlm.v	v23, (s2)
	SIGV	v23

	#---- done: terminate spike via HTIF, then park for DoomV's -break
# Label before the HTIF write, not after: writing tohost terminates spike
# immediately, so anything past this point never executes there. spike is
# stopped here to dump the signature; DoomV, which ignores tohost, runs on
# to the spin loop below and is stopped by -break.
.global sig_done
sig_done:
	la	t0, tohost
	li	t1, 1
	sd	t1, 0(t0)
.global rvtest_halt_doomv
rvtest_halt_doomv:
	j	rvtest_halt_doomv

#---------------------------------------------------------------------
	.section .data
	.align 8
testdata_a:
	.dword	0x0123456789abcdef
	.dword	0xfedcba9876543210
	.dword	0x00000000ffffffff
	.dword	0x8000000000000001
testdata_b:
	.dword	0x1111111122222222
	.dword	0x3333333344444444
	.dword	0x5555555566666666
	.dword	0x7777777788888888

	.align 4
	.global begin_signature
begin_signature:
	.fill	1024, 1, 0
	.global end_signature
end_signature:

	.section .tohost, "aw", @progbits
	.align 8
	.global tohost
tohost:
	.dword	0
	.global fromhost
fromhost:
	.dword	0
