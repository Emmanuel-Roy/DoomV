# Scalar floating point

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

Follow operands, rounding mode, result and accrued exception flags separately. A numerically correct result can still have incorrect flags.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [F — single precision floating point](#f--single-precision-floating-point)
- [D — double precision floating point](#d--double-precision-floating-point)
- [Zfa — additional floating point](#zfa--additional-floating-point)
- [Zfhmin — minimal scalar half precision](#zfhmin--minimal-scalar-half-precision)

## F — single precision floating point

32 FP registers use 64-bit storage. F represents binary32 with upper bits set for NaN boxing; D represents binary64 and requires F. Numerical operations use the shared helpers, with Berkeley SoftFloat used for core arithmetic and rounding-sensitive behavior; other conversions and vector paths still contain host arithmetic. This is not a claim that every FP path is independent of the host.

Implementation: [src/extensions/ext_f.cpp](../../src/extensions/ext_f.cpp), [src/extensions/ext_fp_common.hpp](../../src/extensions/ext_fp_common.hpp), [src/extensions/ext_softfloat.hpp](../../src/extensions/ext_softfloat.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `fadd.s` | Add. |
| `fsub.s` | Subtract second operand from first. |
| `fmul.s` | Multiply. |
| `fdiv.s` | Divide first operand by second. |
| `fsqrt.s` | Square root. |
| `fmadd.s` | Fused a×b+c. |
| `fmsub.s` | Fused a×b−c. |
| `fnmsub.s` | Fused −a×b+c. |
| `fnmadd.s` | Fused −a×b−c. |
| `fsgnj.s` | Copy magnitude of a and sign of b. |
| `fsgnjn.s` | Copy magnitude of a and inverted sign of b. |
| `fsgnjx.s` | Copy magnitude of a and XOR the input sign bits. |
| `fmin.s` | Select minimum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `fmax.s` | Select maximum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `feq.s` | Integer result for equality; qNaN compares false without NV, sNaN raises NV. |
| `flt.s` | Integer result for less-than; NaNs compare false and raise NV. |
| `fle.s` | Integer result for less-or-equal; NaNs compare false and raise NV. |
| `fclass.s` | Return a 10-bit classification mask: negative infinity/normal/subnormal/zero, positive zero/subnormal/normal/infinity, signaling/quiet NaN. |
| `flw` | Load FP bits from translated memory; single values are NaN-boxed into 64-bit storage. |
| `fsw` | Store the low 32 raw bits, without arithmetic conversion. |
| `fcvt.w.s` | Convert floating value to signed 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.s.w` | Convert signed 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.wu.s` | Convert floating value to unsigned 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.s.wu` | Convert unsigned 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.l.s` | Convert floating value to signed 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.s.l` | Convert signed 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fcvt.lu.s` | Convert floating value to unsigned 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.s.lu` | Convert unsigned 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fmv.x.w` | Copy raw FP bits to integer register; W sign-extends bit 31, D requires RV64. |
| `fmv.w.x` | Copy raw integer bits to FP register; W is NaN-boxed, D requires RV64. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `fflags` | `0x001` | Accrued exception flags: NX bit 0 inexact, UF bit 1 underflow, OF bit 2 overflow, DZ bit 3 divide-by-zero, NV bit 4 invalid. Arithmetic ORs flags; CSR writes replace them. |
| `frm` | `0x002` | Dynamic rounding mode: 0 RNE, 1 RTZ, 2 RDN, 3 RUP, 4 RMM; instruction rm=7 requests this field. Reserved modes are not extra rounding algorithms. |
| `fcsr` | `0x003` | Combined view: frm in bits 7:5 and fflags in 4:0. Aliases the same dedicated storage. |

`mstatus.FS` tracks floating state (Off/Initial/Clean/Dirty); `mstatus.SD` summarizes dirty extension state. These are fields in existing privileged CSRs, not additional F CSRs. The current `sstatus` mask omits FS/VS/SD, despite historical comments suggesting a fuller alias.

[Back to contents](#contents)

## D — double precision floating point

32 FP registers use 64-bit storage. F represents binary32 with upper bits set for NaN boxing; D represents binary64 and requires F. Numerical operations use the shared helpers, with Berkeley SoftFloat used for core arithmetic and rounding-sensitive behavior; other conversions and vector paths still contain host arithmetic. This is not a claim that every FP path is independent of the host.

Implementation: [src/extensions/ext_d.cpp](../../src/extensions/ext_d.cpp), [src/extensions/ext_fp_common.hpp](../../src/extensions/ext_fp_common.hpp), [src/extensions/ext_softfloat.hpp](../../src/extensions/ext_softfloat.hpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

| Instruction | Operation and relevant details |
|---|---|
| `fadd.d` | Add. |
| `fsub.d` | Subtract second operand from first. |
| `fmul.d` | Multiply. |
| `fdiv.d` | Divide first operand by second. |
| `fsqrt.d` | Square root. |
| `fmadd.d` | Fused a×b+c. |
| `fmsub.d` | Fused a×b−c. |
| `fnmsub.d` | Fused −a×b+c. |
| `fnmadd.d` | Fused −a×b−c. |
| `fsgnj.d` | Copy magnitude of a and sign of b. |
| `fsgnjn.d` | Copy magnitude of a and inverted sign of b. |
| `fsgnjx.d` | Copy magnitude of a and XOR the input sign bits. |
| `fmin.d` | Select minimum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `fmax.d` | Select maximum numeric value; one NaN returns the numeric operand; sNaN raises NV. |
| `feq.d` | Integer result for equality; qNaN compares false without NV, sNaN raises NV. |
| `flt.d` | Integer result for less-than; NaNs compare false and raise NV. |
| `fle.d` | Integer result for less-or-equal; NaNs compare false and raise NV. |
| `fclass.d` | Return a 10-bit classification mask: negative infinity/normal/subnormal/zero, positive zero/subnormal/normal/infinity, signaling/quiet NaN. |
| `fld` | Load 64 raw bits into an FP register. |
| `fsd` | Store 64 raw bits from an FP register. |
| `fcvt.w.d` | Convert floating value to signed 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.d.w` | Convert signed 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.wu.d` | Convert floating value to unsigned 32-bit integer with rounding, invalid-range handling and inexact flags. Word result is sign-extended on RV64. |
| `fcvt.d.wu` | Convert unsigned 32-bit integer to this FP format, rounding if necessary. |
| `fcvt.l.d` | Convert floating value to signed 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.d.l` | Convert signed 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fcvt.lu.d` | Convert floating value to unsigned 64-bit integer with rounding, invalid-range handling and inexact flags. RV64 integer-long form. |
| `fcvt.d.lu` | Convert unsigned 64-bit integer to this FP format, rounding if necessary. RV64 integer-long form. |
| `fmv.x.d` | Copy raw FP bits to integer register; W sign-extends bit 31, D requires RV64. |
| `fmv.d.x` | Copy raw integer bits to FP register; W is NaN-boxed, D requires RV64. |
| `fcvt.s.d` | Round double to single; NaN handling and flags apply. |
| `fcvt.d.s` | Widen single to double; signaling NaN still raises invalid despite exact finite conversion. |

</details>

### CSR effects

No additional CSRs beyond F: `fflags`, `frm`, `fcsr` are shared. D also uses FS/SD state.

[Back to contents](#contents)

## Zfa — additional floating point

The implemented set is the S/D forms below. There are no half/quad Zfa operations or RV32 high-half/pair double moves in this decoder; the flag alone must not be interpreted as every Zfa format.

Implementation: [src/extensions/ext_zfa.cpp](../../src/extensions/ext_zfa.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `fli.s` | Load one of 32 encoded constants; rs1 is an index, not a register value. |
| `fminm.s` | Minimum variant returning canonical NaN if either operand is NaN. |
| `fmaxm.s` | Maximum variant returning canonical NaN if either operand is NaN. |
| `fround.s` | Round to an integral value in the same FP format without reporting inexact for discarded fraction. |
| `froundnx.s` | Round to an integral FP value and report inexact. |
| `fleq.s` | Quiet less-or-equal: qNaN does not raise NV, sNaN does. |
| `fltq.s` | Quiet less-than with the same NaN flag rule. |
| `fli.d` | Load one of 32 encoded constants; rs1 is an index, not a register value. |
| `fminm.d` | Minimum variant returning canonical NaN if either operand is NaN. |
| `fmaxm.d` | Maximum variant returning canonical NaN if either operand is NaN. |
| `fround.d` | Round to an integral value in the same FP format without reporting inexact for discarded fraction. |
| `froundnx.d` | Round to an integral FP value and report inexact. |
| `fleq.d` | Quiet less-or-equal: qNaN does not raise NV, sNaN does. |
| `fltq.d` | Quiet less-than with the same NaN flag rule. |
| `fcvtmod.w.d` | Truncate double toward zero and wrap modulo 2^32, then sign-extend the word result; unlike saturating float-to-integer conversions. |

### CSR effects

Reuses F `fflags`, `frm`, `fcsr`; adds no CSR.

[Back to contents](#contents)

## Zfhmin — minimal scalar half precision

Storage, raw moves and conversions for IEEE binary16. Full Zfh arithmetic such as FADD.H is not implemented. Half conversions use integer significand handling to avoid double rounding.

Implementation: [src/extensions/ext_zfhmin.cpp](../../src/extensions/ext_zfhmin.cpp), [src/extensions/ext_fp16.hpp](../../src/extensions/ext_fp16.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `flh` | Load 16 raw bits and NaN-box into the FP register. |
| `fsh` | Store low 16 raw bits; do not reinterpret bad NaN boxing as canonical NaN for this raw store. |
| `fmv.x.h` | Copy low 16 raw bits to x register and sign-extend. |
| `fmv.h.x` | Copy low 16 x-register bits into a NaN-boxed FP register. |
| `fcvt.s.h` | Widen binary16 to binary32; finite values are exact. |
| `fcvt.h.s` | Round binary32 to binary16 and accrue conversion flags. |
| `fcvt.d.h` | Widen binary16 to binary64, involving D. |
| `fcvt.h.d` | Round binary64 directly to binary16, avoiding a binary32 intermediate. |

### CSR effects

Shares F CSRs; no new CSR.

[Back to contents](#contents)
