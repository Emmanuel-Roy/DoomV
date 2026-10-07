# The extensions DoomV switches with -march beside V and Sv39, lock-stepped
# against Sail with each one on and off (tools/verification/ext_switches.py).
# One instruction or probe per extension; nothing here checks a value
# itself. With an extension off its instruction is illegal, and the
# handler steps over it, so the trace shows whether each side trapped.
#
#   Zvfh, Zvfhmin, Zvfbfmin, Zvfbfwma   vector FP at SEW=16 (and int8 -> f16)
#   Zvkb, Zvbb, Zvbc                     vector bit manipulation, carry-less multiply
#   Zvkg, Zvkned, Zvknha, Zvknhb         GHASH, AES, SHA-256, SHA-512
#   Zvksed, Zvksh                        SM4, SM3
#   Sv48, Sv57                           satp, vsatp and hgatp keep the mode or not
#   Svadu                                menvcfg.ADUE sticks or not

.equ MSTATUS_FS_VS, (1 << 13) | (1 << 9)

.section .text.init
.global _start
_start:
	la   t0, trap
	csrw mtvec, t0
	li   t0, MSTATUS_FS_VS
	csrs mstatus, t0

	# ---- half precision and bf16, at SEW=16 ----------------------------------------
	vsetivli x0, 4, e16, m1, ta, ma
	vfadd.vv         v1, v2, v3       # Zvfh
	vfwcvt.f.f.v     v4, v2           # Zvfhmin
	vfncvt.f.f.w     v1, v4           # Zvfhmin
	# The bf16 ones as words: the toolchain does not know Zvfbfmin/Zvfbfwma.
	.word 0x4a269357                  # vfwcvtbf16.f.f.v v6, v2      Zvfbfmin
	.word 0x4a6e90d7                  # vfncvtbf16.f.f.w v1, v6      Zvfbfmin
	.word 0xee311457                  # vfwmaccbf16.vv   v8, v2, v3  Zvfbfwma
	vsetivli x0, 4, e8, m1, ta, ma
	vfwcvt.f.x.v     v10, v2          # Zvfh: int8 -> f16

	# ---- bit manipulation and carry-less multiply ------------------------------------
	vandn.vv  v1, v2, v3              # Zvkb
	vrol.vv   v1, v2, v3              # Zvkb
	vrev8.v   v1, v2                  # Zvkb
	vbrev.v   v1, v2                  # Zvbb
	vclz.v    v1, v2                  # Zvbb
	vwsll.vv  v12, v2, v3             # Zvbb
	vsetivli x0, 2, e64, m1, ta, ma
	vclmul.vv  v1, v2, v3             # Zvbc
	vclmulh.vv v1, v2, v3             # Zvbc

	# ---- crypto: element groups of four 32-bit elements -----------------------------
	vsetivli x0, 4, e32, m1, ta, ma
	vghsh.vv   v1, v2, v3             # Zvkg
	vgmul.vv   v1, v2                 # Zvkg
	vaesef.vv  v1, v2                 # Zvkned
	vaeskf1.vi v1, v2, 1              # Zvkned
	vsha2ms.vv v1, v2, v3             # Zvknha
	vsm4k.vi   v1, v2, 0              # Zvksed
	vsm4r.vv   v1, v2                 # Zvksed
	vsetivli x0, 8, e32, m2, ta, ma
	vsm3me.vv  v4, v6, v8             # Zvksh: groups of eight
	vsetivli x0, 4, e64, m2, ta, ma
	vsha2ms.vv v4, v6, v8             # Zvknhb: SHA-512, SEW=64

	# ---- virtual memory -------------------------------------------------------------
	li   t0, (9 << 60)
	csrw satp, t0                     # Sv48
	csrr a0, satp
	li   t0, (10 << 60)
	csrw satp, t0                     # Sv57
	csrr a1, satp
	csrw satp, zero
	li   t0, (9 << 60)
	csrw vsatp, t0
	csrr a2, vsatp
	li   t0, (10 << 60)
	csrw hgatp, t0                    # Sv57x4
	csrr a3, hgatp
	li   t0, (9 << 60)
	csrw hgatp, t0                    # Sv48x4
	csrr a4, hgatp
	csrw hgatp, zero
	csrw vsatp, zero
	li   t0, (1 << 61)
	csrs menvcfg, t0                  # ADUE: Svadu
	csrr a5, menvcfg

	# ---- done ----------------------------------------------------------------------
	li   t0, 1
	la   t1, tohost
	sd   t0, 0(t1)
1:	j    1b

	.align 2
trap:
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
