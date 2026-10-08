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
#   Zacas, Zabha                         amocas at every width, the byte and halfword AMOs

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

	# ---- Zacas and Zabha ---------------------------------------------------------------
	# As words: the toolchain does not know Zacas or Zabha. AMOs take no offset.
	la   s0, amo_data
	li   t0, 0x1122334455667788
	sd   t0, 0(s0)
	sd   t0, 8(s0)
	li   a0, 0x55667788
	li   a1, 0x0badf00d
	.word 0x28B4252F                  # amocas.w a0, a1, (s0)     Zacas: matches, stores
	li   a0, 1
	.word 0x28B4252F                  # amocas.w a0, a1, (s0)     Zacas: does not match, no store
	ld   a3, 8(s0)
	li   a4, 0x7777
	.word 0x28E436AF                  # amocas.d a3, a4, (s0)     Zacas: doubleword
	li   s2, 0x1234
	li   s3, 0x99
	ld   a6, 0(s0)
	ld   a7, 8(s0)
	.word 0x2924482F                  # amocas.q a6, s2, (s0)     Zacas: quadword, pairs a6/a7 and s2/s3
	.word 0x293447AF                  # amocas.q a5, s3, (s0)     odd pairs: illegal
	li   a0, 0x7f
	.word 0x00A407AF                  # amoadd.b  a5, a0, (s0)    Zabha
	.word 0x80A417AF                  # amomin.h  a5, a0, (s0)    Zabha
	addi t1, s0, 1
	.word 0xE0A307AF                  # amomaxu.b a5, a0, (t1)    Zabha
	addi t1, s0, 2
	.word 0x08A317AF                  # amoswap.h a5, a0, (t1)    Zabha
	li   a0, 0
	addi t1, s0, 3
	.word 0x28B3052F                  # amocas.b  a0, a1, (t1)    Zabha and Zacas
	addi t1, s0, 4
	.word 0x28B3152F                  # amocas.h  a0, a1, (t1)    Zabha and Zacas
	.word 0x1004052F                  # lr.b a0, (s0)             never an instruction

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
.align 4
amo_data: .dword 0, 0
.global tohost
tohost:   .dword 0
.global fromhost
fromhost: .dword 0
