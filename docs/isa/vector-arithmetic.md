# Vector arithmetic and permutations

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

Use the family sections to find operations; expand a table only when you need individual mnemonics. Masking, tail policy and restart state are part of the result.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [V — integer and fixed-point instructions](#v--integer-and-fixed-point-instructions)
- [V — permutations masks and reductions](#v--permutations-masks-and-reductions)
- [V — floating-point instructions](#v--floating-point-instructions)
- [Zvfhmin — minimal vector half conversion](#zvfhmin--minimal-vector-half-conversion)
- [Zvbb — vector bit manipulation](#zvbb--vector-bit-manipulation)

## V — integer and fixed-point instructions

Arithmetic runs element by element through shared iteration helpers. Tail/mask-undisturbed behavior leaves old bytes, an allowed choice for many agnostic results; it does not establish comprehensive agnostic-policy validation. Widened integer paths are limited by the helper widths, particularly at SEW=64.

Implementation: [src/extensions/ext_v_int.cpp](../../src/extensions/ext_v_int.cpp), [src/extensions/ext_v_muldiv.cpp](../../src/extensions/ext_v_muldiv.cpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

| Instruction | Operation and relevant details |
|---|---|
| `vadd.vv` | Elementwise addition modulo SEW. |
| `vadd.vx` | Elementwise addition modulo SEW. |
| `vadd.vi` | Elementwise addition modulo SEW. |
| `vsub.vv` | vs2 minus second operand. |
| `vsub.vx` | vs2 minus second operand. |
| `vrsub.vx` | Scalar/immediate minus vs2. |
| `vrsub.vi` | Scalar/immediate minus vs2. |
| `vminu.vv` | Unsigned minimum. |
| `vminu.vx` | Unsigned minimum. |
| `vmin.vv` | Signed minimum. |
| `vmin.vx` | Signed minimum. |
| `vmaxu.vv` | Unsigned maximum. |
| `vmaxu.vx` | Unsigned maximum. |
| `vmax.vv` | Signed maximum. |
| `vmax.vx` | Signed maximum. |
| `vand.vv` | Bitwise AND. |
| `vand.vx` | Bitwise AND. |
| `vand.vi` | Bitwise AND. |
| `vor.vv` | Bitwise OR. |
| `vor.vx` | Bitwise OR. |
| `vor.vi` | Bitwise OR. |
| `vxor.vv` | Bitwise XOR. |
| `vxor.vx` | Bitwise XOR. |
| `vxor.vi` | Bitwise XOR. |
| `vadc.vvm` | Add with carry-in from v0; v0 supplies data, not an execution mask. |
| `vadc.vxm` | Add with carry-in from v0; v0 supplies data, not an execution mask. |
| `vadc.vim` | Add with carry-in from v0; v0 supplies data, not an execution mask. |
| `vmadc.vvm` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vxm` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vim` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vv` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vx` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vmadc.vi` | Produce carry-out mask; m suffix includes v0 carry-in. |
| `vsbc.vvm` | Subtract with v0 borrow-in. |
| `vsbc.vxm` | Subtract with v0 borrow-in. |
| `vmsbc.vvm` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmsbc.vxm` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmsbc.vv` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmsbc.vx` | Produce borrow-out mask; m suffix includes v0 borrow-in. |
| `vmerge.vvm` | Select source per v0 bit. |
| `vmerge.vxm` | Select source per v0 bit. |
| `vmerge.vim` | Select source per v0 bit. |
| `vmv.v.v` | Copy/broadcast source to active destination elements. |
| `vmv.v.x` | Copy/broadcast source to active destination elements. |
| `vmv.v.i` | Copy/broadcast source to active destination elements. |
| `vmseq.vv` | Produce equality mask. |
| `vmseq.vx` | Produce equality mask. |
| `vmseq.vi` | Produce equality mask. |
| `vmsne.vv` | Produce inequality mask. |
| `vmsne.vx` | Produce inequality mask. |
| `vmsne.vi` | Produce inequality mask. |
| `vmsltu.vv` | Produce unsigned less-than mask. |
| `vmsltu.vx` | Produce unsigned less-than mask. |
| `vmslt.vv` | Produce signed less-than mask. |
| `vmslt.vx` | Produce signed less-than mask. |
| `vmsleu.vv` | Produce unsigned less-or-equal mask. |
| `vmsleu.vx` | Produce unsigned less-or-equal mask. |
| `vmsleu.vi` | Produce unsigned less-or-equal mask. |
| `vmsle.vv` | Produce signed less-or-equal mask. |
| `vmsle.vx` | Produce signed less-or-equal mask. |
| `vmsle.vi` | Produce signed less-or-equal mask. |
| `vmsgtu.vx` | Produce unsigned greater-than mask. |
| `vmsgtu.vi` | Produce unsigned greater-than mask. |
| `vmsgt.vx` | Produce signed greater-than mask. |
| `vmsgt.vi` | Produce signed greater-than mask. |
| `vsll.vv` | Logical left shift, count masked to element width. |
| `vsll.vx` | Logical left shift, count masked to element width. |
| `vsll.vi` | Logical left shift, count masked to element width. |
| `vsrl.vv` | Logical right shift. |
| `vsrl.vx` | Logical right shift. |
| `vsrl.vi` | Logical right shift. |
| `vsra.vv` | Arithmetic right shift. |
| `vsra.vx` | Arithmetic right shift. |
| `vsra.vi` | Arithmetic right shift. |
| `vsaddu.vv` | Saturating unsigned add, sets vxsat when clamped. |
| `vsaddu.vx` | Saturating unsigned add, sets vxsat when clamped. |
| `vsaddu.vi` | Saturating unsigned add, sets vxsat when clamped. |
| `vsadd.vv` | Saturating signed add, sets vxsat when clamped. |
| `vsadd.vx` | Saturating signed add, sets vxsat when clamped. |
| `vsadd.vi` | Saturating signed add, sets vxsat when clamped. |
| `vssubu.vv` | Saturating unsigned subtract. |
| `vssubu.vx` | Saturating unsigned subtract. |
| `vssub.vv` | Saturating signed subtract. |
| `vssub.vx` | Saturating signed subtract. |
| `vaaddu.vv` | Unsigned averaging add using vxrm. |
| `vaaddu.vx` | Unsigned averaging add using vxrm. |
| `vaadd.vv` | Signed averaging add using vxrm. |
| `vaadd.vx` | Signed averaging add using vxrm. |
| `vasubu.vv` | Unsigned averaging subtract using vxrm. |
| `vasubu.vx` | Unsigned averaging subtract using vxrm. |
| `vasub.vv` | Signed averaging subtract using vxrm. |
| `vasub.vx` | Signed averaging subtract using vxrm. |
| `vsmul.vv` | Signed fractional multiply with rounding and saturation. |
| `vsmul.vx` | Signed fractional multiply with rounding and saturation. |
| `vssrl.vv` | Rounded logical scaling right shift using vxrm. |
| `vssrl.vx` | Rounded logical scaling right shift using vxrm. |
| `vssrl.vi` | Rounded logical scaling right shift using vxrm. |
| `vssra.vv` | Rounded arithmetic scaling right shift. |
| `vssra.vx` | Rounded arithmetic scaling right shift. |
| `vssra.vi` | Rounded arithmetic scaling right shift. |
| `vnsrl.wv` | Narrow double-width source by logical right shift. |
| `vnsrl.wx` | Narrow double-width source by logical right shift. |
| `vnsrl.wi` | Narrow double-width source by logical right shift. |
| `vnsra.wv` | Narrow double-width source by arithmetic right shift. |
| `vnsra.wx` | Narrow double-width source by arithmetic right shift. |
| `vnsra.wi` | Narrow double-width source by arithmetic right shift. |
| `vnclipu.wv` | Round and saturate wider unsigned input to SEW. |
| `vnclipu.wx` | Round and saturate wider unsigned input to SEW. |
| `vnclipu.wi` | Round and saturate wider unsigned input to SEW. |
| `vnclip.wv` | Round and saturate wider signed input to SEW. |
| `vnclip.wx` | Round and saturate wider signed input to SEW. |
| `vnclip.wi` | Round and saturate wider signed input to SEW. |
| `vmul.vv` | Low product bits. |
| `vmul.vx` | Low product bits. |
| `vmulh.vv` | High signed × signed product. |
| `vmulh.vx` | High signed × signed product. |
| `vmulhu.vv` | High unsigned × unsigned product. |
| `vmulhu.vx` | High unsigned × unsigned product. |
| `vmulhsu.vv` | High signed vs2 × unsigned second operand product. |
| `vmulhsu.vx` | High signed vs2 × unsigned second operand product. |
| `vdiv.vv` | Signed quotient with architectural exceptional operands. |
| `vdiv.vx` | Signed quotient with architectural exceptional operands. |
| `vdivu.vv` | Unsigned quotient. |
| `vdivu.vx` | Unsigned quotient. |
| `vrem.vv` | Signed remainder. |
| `vrem.vx` | Signed remainder. |
| `vremu.vv` | Unsigned remainder. |
| `vremu.vx` | Unsigned remainder. |
| `vmadd.vv` | vd = operand1 × old vd + vs2. |
| `vmadd.vx` | vd = operand1 × old vd + vs2. |
| `vnmsub.vv` | vd = −operand1 × old vd + vs2. |
| `vnmsub.vx` | vd = −operand1 × old vd + vs2. |
| `vmacc.vv` | vd = operand1 × vs2 + old vd. |
| `vmacc.vx` | vd = operand1 × vs2 + old vd. |
| `vnmsac.vv` | vd = −operand1 × vs2 + old vd. |
| `vnmsac.vx` | vd = −operand1 × vs2 + old vd. |
| `vwaddu.vv` | Zero-extend operands and add into double-width destination. |
| `vwaddu.vx` | Zero-extend operands and add into double-width destination. |
| `vwaddu.wv` | Zero-extend operands and add into double-width destination. |
| `vwaddu.wx` | Zero-extend operands and add into double-width destination. |
| `vwadd.vv` | Sign-extend operands and add into double-width destination. |
| `vwadd.vx` | Sign-extend operands and add into double-width destination. |
| `vwadd.wv` | Sign-extend operands and add into double-width destination. |
| `vwadd.wx` | Sign-extend operands and add into double-width destination. |
| `vwsubu.vv` | Widening unsigned subtraction. |
| `vwsubu.vx` | Widening unsigned subtraction. |
| `vwsubu.wv` | Widening unsigned subtraction. |
| `vwsubu.wx` | Widening unsigned subtraction. |
| `vwsub.vv` | Widening signed subtraction. |
| `vwsub.vx` | Widening signed subtraction. |
| `vwsub.wv` | Widening signed subtraction. |
| `vwsub.wx` | Widening signed subtraction. |
| `vwmulu.vv` | Full widening unsigned product. |
| `vwmulu.vx` | Full widening unsigned product. |
| `vwmul.vv` | Full widening signed product. |
| `vwmul.vx` | Full widening signed product. |
| `vwmulsu.vv` | Full signed vs2 × unsigned second operand product. |
| `vwmulsu.vx` | Full signed vs2 × unsigned second operand product. |
| `vwmaccu.vv` | Widening unsigned product accumulated into wide vd. |
| `vwmaccu.vx` | Widening unsigned product accumulated into wide vd. |
| `vwmacc.vv` | Widening signed product accumulated into wide vd. |
| `vwmacc.vx` | Widening signed product accumulated into wide vd. |
| `vwmaccsu.vv` | Widening signed/unsigned multiply accumulate. |
| `vwmaccsu.vx` | Widening signed/unsigned multiply accumulate. |
| `vwmaccus.vx` | Widening unsigned vector × signed scalar accumulate. |

</details>

### CSR effects

Uses vl/vtype/vstart and masking. Fixed-point rounding uses vxrm; saturation ORs vxsat. No additional CSRs.

[Back to contents](#contents)

## V — permutations masks and reductions

These operations move data or combine lanes; they are not ordinary independent-lane arithmetic. Source/destination overlap and restart restrictions matter. VRGATHEREI16 shares a funct6 slot with slide-up, distinguished by funct3.

Implementation: [src/extensions/ext_v_perm.cpp](../../src/extensions/ext_v_perm.cpp), [src/extensions/ext_v_mask.cpp](../../src/extensions/ext_v_mask.cpp), [src/extensions/ext_v_reduce.cpp](../../src/extensions/ext_v_reduce.cpp), [src/extensions/ext_v.cpp](../../src/extensions/ext_v.cpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

| Instruction | Operation and relevant details |
|---|---|
| `vrgather.vv` | Select source by element index; out-of-range source index yields zero. |
| `vrgather.vx` | Select source by element index; out-of-range source index yields zero. |
| `vrgather.vi` | Select source by element index; out-of-range source index yields zero. |
| `vrgatherei16.vv` | Gather using 16-bit vector indices; funct6=0x0e with OPIVV distinguishes it from scalar/immediate slide-up. |
| `vslideup.vx` | Move source elements to higher indices; low destinations are preserved. |
| `vslideup.vi` | Move source elements to higher indices; low destinations are preserved. |
| `vslidedown.vx` | Move source elements to lower indices; beyond source range yields zero. |
| `vslidedown.vi` | Move source elements to lower indices; beyond source range yields zero. |
| `vslide1up.vx` | Shift up one and insert integer scalar at element zero. |
| `vslide1down.vx` | Shift down one and insert integer scalar at the last active element. |
| `vcompress.vm` | Pack elements selected by a mask into consecutive destination elements. |
| `vmv1r.v` | Copy 1 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv2r.v` | Copy 2 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv4r.v` | Copy 4 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv8r.v` | Copy 8 whole vector register(s); independent of ordinary element arithmetic. |
| `vmv.x.s` | Sign-extend vector element 0 to integer destination. |
| `vmv.s.x` | Copy integer scalar into vector element 0. |
| `vcpop.m` | Count set mask bits over active range into integer destination. |
| `vfirst.m` | Index of first set mask bit, or -1 if none. |
| `vmsbf.m` | Mask of positions before the first set source bit. |
| `vmsif.m` | Mask through and including the first set source bit. |
| `vmsof.m` | Mask selecting only the first set source bit. |
| `viota.m` | Prefix count of set source-mask bits. |
| `vid.v` | Write each element index. |
| `vmand.mm` | Boolean mask operation: a AND b. |
| `vmandn.mm` | Boolean mask operation: a AND NOT b. |
| `vmor.mm` | Boolean mask operation: a OR b. |
| `vmorn.mm` | Boolean mask operation: a OR NOT b. |
| `vmxor.mm` | Boolean mask operation: a XOR b. |
| `vmxnor.mm` | Boolean mask operation: NOT(a XOR b). |
| `vmnand.mm` | Boolean mask operation: NOT(a AND b). |
| `vmnor.mm` | Boolean mask operation: NOT(a OR b). |
| `vzext.vf2` | Zero-extend elements from SEW/2 to SEW; source width must remain supported. |
| `vsext.vf2` | Sign-extend elements from SEW/2 to SEW; source width must remain supported. |
| `vzext.vf4` | Zero-extend elements from SEW/4 to SEW; source width must remain supported. |
| `vsext.vf4` | Sign-extend elements from SEW/4 to SEW; source width must remain supported. |
| `vzext.vf8` | Zero-extend elements from SEW/8 to SEW; source width must remain supported. |
| `vsext.vf8` | Sign-extend elements from SEW/8 to SEW; source width must remain supported. |
| `vredsum.vs` | Reduce active vs2 elements with scalar seed vs1[0] using sum; place result in vd[0]. |
| `vredand.vs` | Reduce active vs2 elements with scalar seed vs1[0] using bitwise AND; place result in vd[0]. |
| `vredor.vs` | Reduce active vs2 elements with scalar seed vs1[0] using bitwise OR; place result in vd[0]. |
| `vredxor.vs` | Reduce active vs2 elements with scalar seed vs1[0] using bitwise XOR; place result in vd[0]. |
| `vredminu.vs` | Reduce active vs2 elements with scalar seed vs1[0] using unsigned minimum; place result in vd[0]. |
| `vredmin.vs` | Reduce active vs2 elements with scalar seed vs1[0] using signed minimum; place result in vd[0]. |
| `vredmaxu.vs` | Reduce active vs2 elements with scalar seed vs1[0] using unsigned maximum; place result in vd[0]. |
| `vredmax.vs` | Reduce active vs2 elements with scalar seed vs1[0] using signed maximum; place result in vd[0]. |
| `vwredsumu.vs` | Reduce active vs2 elements with scalar seed vs1[0] using widening unsigned sum; place result in vd[0]. |
| `vwredsum.vs` | Reduce active vs2 elements with scalar seed vs1[0] using widening signed sum; place result in vd[0]. |

</details>

### CSR effects

Uses existing vector state; scalar-result forms write x registers. No new CSR.

[Back to contents](#contents)

## V — floating-point instructions

Ordinary vector FP uses SEW 32/64. SEW=16 admits only two Zvfhmin conversion subcodes; other half operations return without doing arithmetic. The executor, not the display-name table, determines dispatch: several displayed funct6 labels disagree with execution cases. These tables explain the architectural names; they do not assert that the current decoder rejects every non-architectural operand form.

Implementation: [src/extensions/ext_v_fp.cpp](../../src/extensions/ext_v_fp.cpp), [src/extensions/ext_v.cpp](../../src/extensions/ext_v.cpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

| Instruction | Operation and relevant details |
|---|---|
| `vfadd.vv` | Floating add. |
| `vfadd.vf` | Floating add. |
| `vfsub.vv` | vs2 minus operand. |
| `vfsub.vf` | vs2 minus operand. |
| `vfrsub.vf` | Scalar minus vector. |
| `vfmul.vv` | Floating multiply. |
| `vfmul.vf` | Floating multiply. |
| `vfdiv.vv` | Vector divided by operand. |
| `vfdiv.vf` | Vector divided by operand. |
| `vfrdiv.vf` | Scalar divided by vector. |
| `vfmin.vv` | Numeric floating minimum. |
| `vfmin.vf` | Numeric floating minimum. |
| `vfmax.vv` | Numeric floating maximum. |
| `vfmax.vf` | Numeric floating maximum. |
| `vfsgnj.vv` | Copy second operand sign. |
| `vfsgnj.vf` | Copy second operand sign. |
| `vfsgnjn.vv` | Copy inverted second operand sign. |
| `vfsgnjn.vf` | Copy inverted second operand sign. |
| `vfsgnjx.vv` | XOR signs. |
| `vfsgnjx.vf` | XOR signs. |
| `vmfeq.vv` | Equality mask. |
| `vmfeq.vf` | Equality mask. |
| `vmfne.vv` | Inequality mask, including unordered NaN cases. |
| `vmfne.vf` | Inequality mask, including unordered NaN cases. |
| `vmflt.vv` | Less-than mask. |
| `vmflt.vf` | Less-than mask. |
| `vmfle.vv` | Less-or-equal mask. |
| `vmfle.vf` | Less-or-equal mask. |
| `vmfgt.vf` | Greater-than mask. |
| `vmfge.vf` | Greater-or-equal mask. |
| `vfmerge.vfm` | Select scalar or vector with mask. |
| `vfmv.v.f` | Broadcast FP scalar. |
| `vfslide1up.vf` | Slide up one and insert FP scalar. |
| `vfslide1down.vf` | Slide down one and insert FP scalar. |
| `vfsqrt.v` | Square root per active element. |
| `vfclass.v` | FP classification mask per element. |
| `vfrec7.v` | Architecturally specified 7-bit reciprocal estimate; DoomV computes a division approximation instead, so bit-exact conformance is not established. |
| `vfrsqrt7.v` | Architecturally specified reciprocal-square-root estimate; DoomV uses square roots and division rather than the prescribed estimate table. |
| `vfmv.f.s` | Move vector element 0 into an FP scalar. |
| `vfmv.s.f` | Move FP scalar into vector element 0. |
| `vfmadd.vv` | Fused expression a×old vd+vs2, with one arithmetic rounding. |
| `vfmadd.vf` | Fused expression a×old vd+vs2, with one arithmetic rounding. |
| `vfnmadd.vv` | Fused expression −a×old vd−vs2, with one arithmetic rounding. |
| `vfnmadd.vf` | Fused expression −a×old vd−vs2, with one arithmetic rounding. |
| `vfmsub.vv` | Fused expression a×old vd−vs2, with one arithmetic rounding. |
| `vfmsub.vf` | Fused expression a×old vd−vs2, with one arithmetic rounding. |
| `vfnmsub.vv` | Fused expression −a×old vd+vs2, with one arithmetic rounding. |
| `vfnmsub.vf` | Fused expression −a×old vd+vs2, with one arithmetic rounding. |
| `vfmacc.vv` | Fused expression a×vs2+old vd, with one arithmetic rounding. |
| `vfmacc.vf` | Fused expression a×vs2+old vd, with one arithmetic rounding. |
| `vfnmacc.vv` | Fused expression −a×vs2−old vd, with one arithmetic rounding. |
| `vfnmacc.vf` | Fused expression −a×vs2−old vd, with one arithmetic rounding. |
| `vfmsac.vv` | Fused expression a×vs2−old vd, with one arithmetic rounding. |
| `vfmsac.vf` | Fused expression a×vs2−old vd, with one arithmetic rounding. |
| `vfnmsac.vv` | Fused expression −a×vs2+old vd, with one arithmetic rounding. |
| `vfnmsac.vf` | Fused expression −a×vs2+old vd, with one arithmetic rounding. |
| `vfwadd.vv` | Widening add; .w form has an already-wide vs2. |
| `vfwadd.vf` | Widening add; .w form has an already-wide vs2. |
| `vfwadd.wv` | Widening add; .w form has an already-wide vs2. |
| `vfwadd.wf` | Widening add; .w form has an already-wide vs2. |
| `vfwsub.vv` | Widening subtract; .w form has an already-wide vs2. |
| `vfwsub.vf` | Widening subtract; .w form has an already-wide vs2. |
| `vfwsub.wv` | Widening subtract; .w form has an already-wide vs2. |
| `vfwsub.wf` | Widening subtract; .w form has an already-wide vs2. |
| `vfwmul.vv` | Widening multiply. |
| `vfwmul.vf` | Widening multiply. |
| `vfwmacc.vv` | Wide a×b+old vd. |
| `vfwmacc.vf` | Wide a×b+old vd. |
| `vfwnmacc.vv` | Wide −a×b−old vd. |
| `vfwnmacc.vf` | Wide −a×b−old vd. |
| `vfwmsac.vv` | Wide a×b−old vd. |
| `vfwmsac.vf` | Wide a×b−old vd. |
| `vfwnmsac.vv` | Wide −a×b+old vd. |
| `vfwnmsac.vf` | Wide −a×b+old vd. |
| `vfredusum.vs` | Unordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfredosum.vs` | Ordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfredmin.vs` | Minimum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfredmax.vs` | Maximum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfwredusum.vs` | Widening unordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfwredosum.vs` | Widening ordered sum reduction seeded by vs1[0], result vd[0]; current reductions use a sequential loop. |
| `vfcvt.xu.f.v` | Floating to unsigned integer; preserve element width. |
| `vfcvt.x.f.v` | Floating to signed integer; preserve element width. |
| `vfcvt.f.xu.v` | Unsigned integer to floating; preserve element width. |
| `vfcvt.f.x.v` | Signed integer to floating; preserve element width. |
| `vfcvt.rtz.xu.f.v` | Floating to unsigned integer, truncating toward zero; preserve element width. |
| `vfcvt.rtz.x.f.v` | Floating to signed integer, truncating toward zero; preserve element width. |
| `vfwcvt.xu.f.v` | Floating to unsigned integer; widen output. |
| `vfwcvt.x.f.v` | Floating to signed integer; widen output. |
| `vfwcvt.f.xu.v` | Unsigned integer to floating; widen output. |
| `vfwcvt.f.x.v` | Signed integer to floating; widen output. |
| `vfwcvt.rtz.xu.f.v` | Floating to unsigned integer, truncating toward zero; widen output. |
| `vfwcvt.rtz.x.f.v` | Floating to signed integer, truncating toward zero; widen output. |
| `vfncvt.xu.f.w` | Floating to unsigned integer; narrow output. |
| `vfncvt.x.f.w` | Floating to signed integer; narrow output. |
| `vfncvt.f.xu.w` | Unsigned integer to floating; narrow output. |
| `vfncvt.f.x.w` | Signed integer to floating; narrow output. |
| `vfncvt.rtz.xu.f.w` | Floating to unsigned integer, truncating toward zero; narrow output. |
| `vfncvt.rtz.x.f.w` | Floating to signed integer, truncating toward zero; narrow output. |
| `vfwcvt.f.f.v` | Widen floating format; includes the half-to-single Zvfhmin case. |
| `vfncvt.f.f.w` | Narrow floating format under frm; includes single-to-half Zvfhmin. |
| `vfncvt.rod.f.f.w` | Narrow with round-to-odd. Present in wider conversion code; current SEW=16 admission gate excludes this subcode. |

</details>

### CSR effects

Shares fflags/frm/fcsr plus V CSRs and FS/VS status. No new FP-vector CSR.

[Back to contents](#contents)

## Zvfhmin — minimal vector half conversion

No independent flag; implemented through V floating conversions. It does not enable full half arithmetic or Zvfh.

Implementation: [src/extensions/ext_v_fp.cpp](../../src/extensions/ext_v_fp.cpp), [src/extensions/ext_fp16.hpp](../../src/extensions/ext_fp16.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `vfwcvt.f.f.v` | At SEW=16, widen binary16 source to binary32 destination. |
| `vfncvt.f.f.w` | At SEW=16, narrow binary32 source to binary16 destination with rounding. |

### CSR effects

Shares floating/vector CSRs. No new CSR.

[Back to contents](#contents)

## Zvbb — vector bit manipulation

No Zvbb runtime flag: V routes these slots directly. Unary slots share funct6 with V sign/zero extension, distinguished by rs1 subcode.

Implementation: [src/extensions/ext_zvbb.cpp](../../src/extensions/ext_zvbb.cpp), [src/extensions/ext_v.cpp](../../src/extensions/ext_v.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `vandn.vv` | Bitwise vs2 AND NOT operand. |
| `vandn.vx` | Bitwise vs2 AND NOT operand. |
| `vror.vv` | Rotate elements right; immediate may use six bits split across instruction fields. |
| `vror.vx` | Rotate elements right; immediate may use six bits split across instruction fields. |
| `vror.vi` | Rotate elements right; immediate may use six bits split across instruction fields. |
| `vrol.vv` | Rotate elements left. |
| `vrol.vx` | Rotate elements left. |
| `vwsll.vv` | Zero-extend source then shift into double-width destination. |
| `vwsll.vx` | Zero-extend source then shift into double-width destination. |
| `vwsll.vi` | Zero-extend source then shift into double-width destination. |
| `vbrev8.v` | Reverse bits independently in each byte. |
| `vrev8.v` | Reverse bytes within each element. |
| `vbrev.v` | Reverse all bits in each element. |
| `vclz.v` | Count leading zeros per element. |
| `vctz.v` | Count trailing zeros per element. |
| `vcpop.v` | Count set bits per element. |

### CSR effects

Existing V state only; no new CSR.

[Back to contents](#contents)
