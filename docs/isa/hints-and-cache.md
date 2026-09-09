# Hints, ordering and cache-block operations

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

An instruction that has no visible work on this interpreter can still have encoding and privilege requirements. CBO.ZERO is different: it actually changes memory.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [Zifencei — instruction-fetch synchronization](#zifencei--instruction-fetch-synchronization)
- [Zihintpause — pause hint](#zihintpause--pause-hint)
- [Zihintntl — non-temporal locality hints](#zihintntl--non-temporal-locality-hints)
- [Zimop — may-be operations](#zimop--may-be-operations)
- [Zcmop — compressed may-be operations](#zcmop--compressed-may-be-operations)
- [Zicbom — cache-block management](#zicbom--cache-block-management)
- [Zicbop — prefetch hints](#zicbop--prefetch-hints)
- [Zicboz — block zero](#zicboz--block-zero)
- [Zawrs — wait on reservation set](#zawrs--wait-on-reservation-set)

## Zifencei — instruction-fetch synchronization

The decoder cache is not an architectural instruction cache: it rechecks live instruction bytes before using a decode. Thus modified code misses the old tag.

Implementation: [src/extensions/ext_zifencei.cpp](../../src/extensions/ext_zifencei.cpp), [src/riscv_decoder.cpp](../../src/riscv_decoder.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `fence.i` | Make previous stores visible to later instruction fetches on this hart; no extra flush is required by this implementation. |

[Back to contents](#contents)

## Zihintpause — pause hint

Hints express performance intent without requiring blocking.

Implementation: [src/extensions/ext_zihintpause.cpp](../../src/extensions/ext_zihintpause.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `pause` | Retire immediately. With its flag off, the encoding can retain its legal base FENCE behavior. |

[Back to contents](#contents)

## Zihintntl — non-temporal locality hints

No cache placement policy is modeled. The explicit hint helper handles compressed forms; normal ADD-to-x0 semantics cover the wide forms.

Implementation: [src/extensions/ext_zihintntl.cpp](../../src/extensions/ext_zihintntl.cpp), [src/extensions/ext_c.cpp](../../src/extensions/ext_c.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `ntl.p1` | No temporal locality within the innermost private cache; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.p1` | No temporal locality within the innermost private cache; compressed hint has an explicit decoder name. |
| `ntl.pall` | No temporal locality in private caches; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.pall` | No temporal locality in private caches; compressed hint has an explicit decoder name. |
| `ntl.s1` | No temporal locality through the innermost shared cache; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.s1` | No temporal locality through the innermost shared cache; compressed hint has an explicit decoder name. |
| `ntl.all` | No temporal locality through all cache levels; 32-bit ADD-to-x0 hint encoding has no observable effect here. |
| `c.ntl.all` | No temporal locality through all cache levels; compressed hint has an explicit decoder name. |

[Back to contents](#contents)

## Zimop — may-be operations

Reserved expansion space with defined fallback semantics. Numeric members are separately listed because future extensions may assign meaning to individual encodings.

Implementation: [src/extensions/ext_zimop.cpp](../../src/extensions/ext_zimop.cpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

| Instruction | Operation and relevant details |
|---|---|
| `mop.r.0` | One source operand; current fallback writes zero to rd. |
| `mop.r.1` | One source operand; current fallback writes zero to rd. |
| `mop.r.2` | One source operand; current fallback writes zero to rd. |
| `mop.r.3` | One source operand; current fallback writes zero to rd. |
| `mop.r.4` | One source operand; current fallback writes zero to rd. |
| `mop.r.5` | One source operand; current fallback writes zero to rd. |
| `mop.r.6` | One source operand; current fallback writes zero to rd. |
| `mop.r.7` | One source operand; current fallback writes zero to rd. |
| `mop.r.8` | One source operand; current fallback writes zero to rd. |
| `mop.r.9` | One source operand; current fallback writes zero to rd. |
| `mop.r.10` | One source operand; current fallback writes zero to rd. |
| `mop.r.11` | One source operand; current fallback writes zero to rd. |
| `mop.r.12` | One source operand; current fallback writes zero to rd. |
| `mop.r.13` | One source operand; current fallback writes zero to rd. |
| `mop.r.14` | One source operand; current fallback writes zero to rd. |
| `mop.r.15` | One source operand; current fallback writes zero to rd. |
| `mop.r.16` | One source operand; current fallback writes zero to rd. |
| `mop.r.17` | One source operand; current fallback writes zero to rd. |
| `mop.r.18` | One source operand; current fallback writes zero to rd. |
| `mop.r.19` | One source operand; current fallback writes zero to rd. |
| `mop.r.20` | One source operand; current fallback writes zero to rd. |
| `mop.r.21` | One source operand; current fallback writes zero to rd. |
| `mop.r.22` | One source operand; current fallback writes zero to rd. |
| `mop.r.23` | One source operand; current fallback writes zero to rd. |
| `mop.r.24` | One source operand; current fallback writes zero to rd. |
| `mop.r.25` | One source operand; current fallback writes zero to rd. |
| `mop.r.26` | One source operand; current fallback writes zero to rd. |
| `mop.r.27` | One source operand; current fallback writes zero to rd. |
| `mop.r.28` | One source operand; current fallback writes zero to rd. |
| `mop.r.29` | One source operand; current fallback writes zero to rd. |
| `mop.r.30` | One source operand; current fallback writes zero to rd. |
| `mop.r.31` | One source operand; current fallback writes zero to rd. |
| `mop.rr.0` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.1` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.2` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.3` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.4` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.5` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.6` | Two source operands; current fallback writes zero to rd. |
| `mop.rr.7` | Two source operands; current fallback writes zero to rd. |

</details>

[Back to contents](#contents)

## Zcmop — compressed may-be operations

Compressed expansion space. These encodings have no destination register and leave all registers unchanged; the helper only advances PC. Parser enabling Zcmop also enables Zimop.

Implementation: [src/extensions/ext_zcmop.cpp](../../src/extensions/ext_zcmop.cpp), [src/extensions/ext_c.cpp](../../src/extensions/ext_c.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `c.mop.1` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.3` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.5` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.7` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.9` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.11` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.13` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |
| `c.mop.15` | DoomV names this C.MOP and runs the compressed fallback helper, then advances PC by 2. |

[Back to contents](#contents)

## Zicbom — cache-block management

Block size is 64 bytes. There is no data cache, but these instructions translate an aligned-down address and check read OR write permission, without requiring the PTE dirty bit. Failure uses store-class fault reporting. Environment CBIE/CBCFE controls are not enforced in the current helper; checking page/PMP access is a different layer.

Implementation: [src/extensions/ext_zicbom.cpp](../../src/extensions/ext_zicbom.cpp), [src/mmu.cpp](../../src/mmu.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `cbo.inval` | Invalidate cached copies architecturally; current memory contents remain unchanged after access checks. |
| `cbo.clean` | Write back dirty cached data architecturally; no cache writeback exists here. |
| `cbo.flush` | Clean and invalidate architecturally; no cached state exists here. |

### CSR effects

No new CSR. Architectural control fields occupy `menvcfg.CBIE[5:4]`, `CBCFE[6]` and corresponding lower/virtual environment controls; these are not fully modeled permissions in this snapshot.

[Back to contents](#contents)

## Zicbop — prefetch hints

Encoded in ORI-to-x0 hint space, so disabling the label need not make the encoding illegal.

Implementation: [src/extensions/ext_zicbop.cpp](../../src/extensions/ext_zicbop.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `prefetch.i` | Hint that instruction fetch will use the addressed block; no memory access or prefetch state is generated. |
| `prefetch.r` | Hint that data will be read; retires without effect. |
| `prefetch.w` | Hint that data will be written; retires without effect. |

[Back to contents](#contents)

## Zicboz — block zero

Unlike cache-maintenance hints, zeroing changes guest-visible bytes.

Implementation: [src/extensions/ext_zicboz.cpp](../../src/extensions/ext_zicboz.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `cbo.zero` | Align rs1 down to 64 bytes, then write eight zero doublewords through store translation. Earlier chunks remain zero if a later access faults. The helper currently omits an explicit 8-byte size argument to translate_or_trap. |

### CSR effects

No new CSR; architectural `menvcfg.CBZE[7]` and lower/virtual counterparts control permission, but this executor does not enforce that environment gate.

[Back to contents](#contents)

## Zawrs — wait on reservation set

Both encodings use SYSTEM space and run through a dedicated helper. Immediate completion prevents a single-hart interpreter from waiting indefinitely for another hart.

Implementation: [src/extensions/ext_zawrs.cpp](../../src/extensions/ext_zawrs.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `wrs.nto` | Reservation-set wait without a specified timeout; DoomV returns immediately. |
| `wrs.sto` | Reservation-set wait with short timeout; DoomV returns immediately. |

### CSR effects

No CSR. Reservation state is internal; privilege/timeout controls are not comprehensively modeled.

[Back to contents](#contents)
