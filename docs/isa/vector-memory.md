# Vector configuration and memory

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

Read configuration before the load/store tables. The current vector length, element width and grouping determine how many elements an instruction processes.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [Worked example: three 32-bit elements](#worked-example-three-32-bit-elements)
- [V — vector configuration and execution model](#v--vector-configuration-and-execution-model)
- [V — vector memory instructions](#v--vector-memory-instructions)

## Worked example: three 32-bit elements

`VLEN` is the physical vector-register width; DoomV fixes it at 128 bits.
`SEW` is the selected element width, and `LMUL` describes register grouping.
With `SEW = 32` and `LMUL = 1`, a register can hold four elements:
`VLMAX = 128 / 32 = 4`.

If a legal configuration requests three elements, `vl = 3`. An unmasked
32-bit unit-stride load starting at address `base` reads elements at
`base`, `base + 4`, and `base + 8`. The fourth slot is a tail element; it is
not another requested memory access. A mask can suppress an element within
the first three, and `vstart` can select a later restart position.

These are different questions: how many elements fit (`VLMAX`), how many the
operation covers (`vl`), and which covered elements actually execute (mask and
restart state). Mixing them up can make a vector result look correct while
the instruction performs an extra load or raises an unexpected fault.

Trace [configuration selection](../../src/extensions/ext_v_config.cpp),
[element addressing](../../src/extensions/ext_v_common.hpp), and
[memory operations](../../src/extensions/ext_v_ldst.cpp).

## V — vector configuration and execution model

V is opt-in. `VLEN=128`, supported ordinary element widths are 8/16/32/64 and register groups implement LMUL. For arithmetic, .vv supplies vector operands, .vx an integer scalar, .vi an immediate, .vf an FP scalar. These suffixes are **not** universally interchangeable. The following tables enumerate distinct mnemonic forms; memory families expand their encoded widths and field counts.

Execution comes from funct fields, not the dashboard mnemonic: some display names are stale or generic. Enabled V does not guarantee legality checking for every reserved encoding, overlap, SEW/EMUL combination or restart condition. Several handlers return silently for unsupported cases. Treat the listed operation meaning as the intended architectural operation and consult the limitation notes for the source behavior.

Implementation: [src/extensions/ext_v.cpp](../../src/extensions/ext_v.cpp), [src/extensions/ext_v_config.cpp](../../src/extensions/ext_v_config.cpp), [src/extensions/ext_v_common.hpp](../../src/extensions/ext_v_common.hpp), [src/registers.hpp](../../src/registers.hpp).

| Instruction | Operation and relevant details |
|---|---|
| `vsetvli` | AVL comes from rs1 and vtype from immediate; rd receives selected vl. |
| `vsetivli` | AVL is a 5-bit immediate and vtype is immediate. |
| `vsetvl` | AVL and vtype come from integer registers; useful for restoring saved state. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `vstart` | `0x008` | Index of the element at which execution starts/restarts. Normal completion resets it; ordinary vector memory faults record the failing element. |
| `vxsat` | `0x009` | Sticky fixed-point saturation flag. |
| `vxrm` | `0x00A` | Fixed-point rounding mode: rnu, rne, rdn, rod. |
| `vcsr` | `0x00F` | Combined vxrm[2:1] and vxsat[0] view. |
| `vl` | `0xC20` | Current active element count; written by vset instructions and fault-only-first shortening. |
| `vtype` | `0xC21` | LMUL[2:0], VSEW[5:3], VTA[6], VMA[7], VILL[XLEN-1]. Invalid configuration yields vl=0. |
| `vlenb` | `0xC22` | Read-only 16 bytes: VLEN=128 bits. |

Uses `mstatus.VS` for state enable/dirty tracking. FP vector operations share F flags and rounding. Storage is 32 arrays of 16 bytes; grouping and effective element width are implemented by shared byte-addressing helpers.

[Back to contents](#contents)

## V — vector memory instructions

The common memory loop handles unit, strided, indexed and segmented forms. Segment address is base + element stride + field offset. Indexed operations use a vector of byte offsets; ordered versus unordered variants currently use the same sequential host loop. Masked-off elements skip access.

Fault-only-first element 0 faults normally; a later translation failure shortens vl and completes. The later-element probe calls mmu_translate directly, bypassing the final physical-access wrapper. Ordinary vector accesses also omit a full element size in some wrapper calls. Whole-register/mask paths have distinct loops and must not be assumed to share every ordinary-element restart behavior.

Implementation: [src/extensions/ext_v_ldst.cpp](../../src/extensions/ext_v_ldst.cpp).

<details>
<summary>Expand the full instruction or CSR table</summary>

| Instruction | Operation and relevant details |
|---|---|
| `vle8.v` | Unit-stride load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse8.v` | Unit-stride store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse8.v` | Signed byte-stride load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse8.v` | Signed byte-stride store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei8.v` | Unordered indexed load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei8.v` | Ordered indexed load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei8.v` | Unordered indexed store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei8.v` | Ordered indexed store; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle8ff.v` | Fault-only-first unit-stride load; encoded width 8 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e8.v` | Unit-stride segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e8.v` | Unit-stride segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e8.v` | Strided segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e8.v` | Strided segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei8.v` | Unordered indexed segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei8.v` | Ordered indexed segmented load, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei8.v` | Unordered indexed segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei8.v` | Ordered indexed segmented store, 2 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e8ff.v` | Fault-only-first segmented load of 2 fields of 8-bit elements. |
| `vlseg3e8.v` | Unit-stride segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e8.v` | Unit-stride segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e8.v` | Strided segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e8.v` | Strided segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei8.v` | Unordered indexed segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei8.v` | Ordered indexed segmented load, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei8.v` | Unordered indexed segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei8.v` | Ordered indexed segmented store, 3 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e8ff.v` | Fault-only-first segmented load of 3 fields of 8-bit elements. |
| `vlseg4e8.v` | Unit-stride segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e8.v` | Unit-stride segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e8.v` | Strided segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e8.v` | Strided segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei8.v` | Unordered indexed segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei8.v` | Ordered indexed segmented load, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei8.v` | Unordered indexed segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei8.v` | Ordered indexed segmented store, 4 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e8ff.v` | Fault-only-first segmented load of 4 fields of 8-bit elements. |
| `vlseg5e8.v` | Unit-stride segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e8.v` | Unit-stride segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e8.v` | Strided segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e8.v` | Strided segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei8.v` | Unordered indexed segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei8.v` | Ordered indexed segmented load, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei8.v` | Unordered indexed segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei8.v` | Ordered indexed segmented store, 5 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e8ff.v` | Fault-only-first segmented load of 5 fields of 8-bit elements. |
| `vlseg6e8.v` | Unit-stride segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e8.v` | Unit-stride segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e8.v` | Strided segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e8.v` | Strided segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei8.v` | Unordered indexed segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei8.v` | Ordered indexed segmented load, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei8.v` | Unordered indexed segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei8.v` | Ordered indexed segmented store, 6 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e8ff.v` | Fault-only-first segmented load of 6 fields of 8-bit elements. |
| `vlseg7e8.v` | Unit-stride segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e8.v` | Unit-stride segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e8.v` | Strided segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e8.v` | Strided segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei8.v` | Unordered indexed segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei8.v` | Ordered indexed segmented load, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei8.v` | Unordered indexed segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei8.v` | Ordered indexed segmented store, 7 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e8ff.v` | Fault-only-first segmented load of 7 fields of 8-bit elements. |
| `vlseg8e8.v` | Unit-stride segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e8.v` | Unit-stride segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e8.v` | Strided segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e8.v` | Strided segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei8.v` | Unordered indexed segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei8.v` | Ordered indexed segmented load, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei8.v` | Unordered indexed segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei8.v` | Ordered indexed segmented store, 8 fields, encoded width 8. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e8ff.v` | Fault-only-first segmented load of 8 fields of 8-bit elements. |
| `vle16.v` | Unit-stride load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse16.v` | Unit-stride store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse16.v` | Signed byte-stride load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse16.v` | Signed byte-stride store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei16.v` | Unordered indexed load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei16.v` | Ordered indexed load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei16.v` | Unordered indexed store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei16.v` | Ordered indexed store; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle16ff.v` | Fault-only-first unit-stride load; encoded width 16 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e16.v` | Unit-stride segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e16.v` | Unit-stride segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e16.v` | Strided segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e16.v` | Strided segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei16.v` | Unordered indexed segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei16.v` | Ordered indexed segmented load, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei16.v` | Unordered indexed segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei16.v` | Ordered indexed segmented store, 2 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e16ff.v` | Fault-only-first segmented load of 2 fields of 16-bit elements. |
| `vlseg3e16.v` | Unit-stride segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e16.v` | Unit-stride segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e16.v` | Strided segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e16.v` | Strided segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei16.v` | Unordered indexed segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei16.v` | Ordered indexed segmented load, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei16.v` | Unordered indexed segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei16.v` | Ordered indexed segmented store, 3 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e16ff.v` | Fault-only-first segmented load of 3 fields of 16-bit elements. |
| `vlseg4e16.v` | Unit-stride segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e16.v` | Unit-stride segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e16.v` | Strided segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e16.v` | Strided segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei16.v` | Unordered indexed segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei16.v` | Ordered indexed segmented load, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei16.v` | Unordered indexed segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei16.v` | Ordered indexed segmented store, 4 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e16ff.v` | Fault-only-first segmented load of 4 fields of 16-bit elements. |
| `vlseg5e16.v` | Unit-stride segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e16.v` | Unit-stride segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e16.v` | Strided segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e16.v` | Strided segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei16.v` | Unordered indexed segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei16.v` | Ordered indexed segmented load, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei16.v` | Unordered indexed segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei16.v` | Ordered indexed segmented store, 5 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e16ff.v` | Fault-only-first segmented load of 5 fields of 16-bit elements. |
| `vlseg6e16.v` | Unit-stride segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e16.v` | Unit-stride segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e16.v` | Strided segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e16.v` | Strided segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei16.v` | Unordered indexed segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei16.v` | Ordered indexed segmented load, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei16.v` | Unordered indexed segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei16.v` | Ordered indexed segmented store, 6 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e16ff.v` | Fault-only-first segmented load of 6 fields of 16-bit elements. |
| `vlseg7e16.v` | Unit-stride segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e16.v` | Unit-stride segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e16.v` | Strided segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e16.v` | Strided segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei16.v` | Unordered indexed segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei16.v` | Ordered indexed segmented load, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei16.v` | Unordered indexed segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei16.v` | Ordered indexed segmented store, 7 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e16ff.v` | Fault-only-first segmented load of 7 fields of 16-bit elements. |
| `vlseg8e16.v` | Unit-stride segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e16.v` | Unit-stride segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e16.v` | Strided segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e16.v` | Strided segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei16.v` | Unordered indexed segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei16.v` | Ordered indexed segmented load, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei16.v` | Unordered indexed segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei16.v` | Ordered indexed segmented store, 8 fields, encoded width 16. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e16ff.v` | Fault-only-first segmented load of 8 fields of 16-bit elements. |
| `vle32.v` | Unit-stride load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse32.v` | Unit-stride store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse32.v` | Signed byte-stride load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse32.v` | Signed byte-stride store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei32.v` | Unordered indexed load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei32.v` | Ordered indexed load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei32.v` | Unordered indexed store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei32.v` | Ordered indexed store; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle32ff.v` | Fault-only-first unit-stride load; encoded width 32 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e32.v` | Unit-stride segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e32.v` | Unit-stride segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e32.v` | Strided segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e32.v` | Strided segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei32.v` | Unordered indexed segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei32.v` | Ordered indexed segmented load, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei32.v` | Unordered indexed segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei32.v` | Ordered indexed segmented store, 2 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e32ff.v` | Fault-only-first segmented load of 2 fields of 32-bit elements. |
| `vlseg3e32.v` | Unit-stride segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e32.v` | Unit-stride segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e32.v` | Strided segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e32.v` | Strided segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei32.v` | Unordered indexed segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei32.v` | Ordered indexed segmented load, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei32.v` | Unordered indexed segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei32.v` | Ordered indexed segmented store, 3 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e32ff.v` | Fault-only-first segmented load of 3 fields of 32-bit elements. |
| `vlseg4e32.v` | Unit-stride segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e32.v` | Unit-stride segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e32.v` | Strided segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e32.v` | Strided segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei32.v` | Unordered indexed segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei32.v` | Ordered indexed segmented load, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei32.v` | Unordered indexed segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei32.v` | Ordered indexed segmented store, 4 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e32ff.v` | Fault-only-first segmented load of 4 fields of 32-bit elements. |
| `vlseg5e32.v` | Unit-stride segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e32.v` | Unit-stride segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e32.v` | Strided segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e32.v` | Strided segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei32.v` | Unordered indexed segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei32.v` | Ordered indexed segmented load, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei32.v` | Unordered indexed segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei32.v` | Ordered indexed segmented store, 5 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e32ff.v` | Fault-only-first segmented load of 5 fields of 32-bit elements. |
| `vlseg6e32.v` | Unit-stride segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e32.v` | Unit-stride segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e32.v` | Strided segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e32.v` | Strided segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei32.v` | Unordered indexed segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei32.v` | Ordered indexed segmented load, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei32.v` | Unordered indexed segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei32.v` | Ordered indexed segmented store, 6 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e32ff.v` | Fault-only-first segmented load of 6 fields of 32-bit elements. |
| `vlseg7e32.v` | Unit-stride segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e32.v` | Unit-stride segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e32.v` | Strided segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e32.v` | Strided segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei32.v` | Unordered indexed segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei32.v` | Ordered indexed segmented load, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei32.v` | Unordered indexed segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei32.v` | Ordered indexed segmented store, 7 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e32ff.v` | Fault-only-first segmented load of 7 fields of 32-bit elements. |
| `vlseg8e32.v` | Unit-stride segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e32.v` | Unit-stride segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e32.v` | Strided segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e32.v` | Strided segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei32.v` | Unordered indexed segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei32.v` | Ordered indexed segmented load, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei32.v` | Unordered indexed segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei32.v` | Ordered indexed segmented store, 8 fields, encoded width 32. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e32ff.v` | Fault-only-first segmented load of 8 fields of 32-bit elements. |
| `vle64.v` | Unit-stride load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vse64.v` | Unit-stride store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlse64.v` | Signed byte-stride load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsse64.v` | Signed byte-stride store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vluxei64.v` | Unordered indexed load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vloxei64.v` | Ordered indexed load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsuxei64.v` | Unordered indexed store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vsoxei64.v` | Ordered indexed store; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vle64ff.v` | Fault-only-first unit-stride load; encoded width 64 bits. Indexed width sizes the offset element, while data uses SEW. |
| `vlseg2e64.v` | Unit-stride segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg2e64.v` | Unit-stride segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg2e64.v` | Strided segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg2e64.v` | Strided segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg2ei64.v` | Unordered indexed segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg2ei64.v` | Ordered indexed segmented load, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg2ei64.v` | Unordered indexed segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg2ei64.v` | Ordered indexed segmented store, 2 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg2e64ff.v` | Fault-only-first segmented load of 2 fields of 64-bit elements. |
| `vlseg3e64.v` | Unit-stride segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg3e64.v` | Unit-stride segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg3e64.v` | Strided segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg3e64.v` | Strided segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg3ei64.v` | Unordered indexed segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg3ei64.v` | Ordered indexed segmented load, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg3ei64.v` | Unordered indexed segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg3ei64.v` | Ordered indexed segmented store, 3 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg3e64ff.v` | Fault-only-first segmented load of 3 fields of 64-bit elements. |
| `vlseg4e64.v` | Unit-stride segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg4e64.v` | Unit-stride segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg4e64.v` | Strided segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg4e64.v` | Strided segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg4ei64.v` | Unordered indexed segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg4ei64.v` | Ordered indexed segmented load, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg4ei64.v` | Unordered indexed segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg4ei64.v` | Ordered indexed segmented store, 4 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg4e64ff.v` | Fault-only-first segmented load of 4 fields of 64-bit elements. |
| `vlseg5e64.v` | Unit-stride segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg5e64.v` | Unit-stride segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg5e64.v` | Strided segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg5e64.v` | Strided segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg5ei64.v` | Unordered indexed segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg5ei64.v` | Ordered indexed segmented load, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg5ei64.v` | Unordered indexed segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg5ei64.v` | Ordered indexed segmented store, 5 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg5e64ff.v` | Fault-only-first segmented load of 5 fields of 64-bit elements. |
| `vlseg6e64.v` | Unit-stride segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg6e64.v` | Unit-stride segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg6e64.v` | Strided segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg6e64.v` | Strided segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg6ei64.v` | Unordered indexed segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg6ei64.v` | Ordered indexed segmented load, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg6ei64.v` | Unordered indexed segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg6ei64.v` | Ordered indexed segmented store, 6 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg6e64ff.v` | Fault-only-first segmented load of 6 fields of 64-bit elements. |
| `vlseg7e64.v` | Unit-stride segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg7e64.v` | Unit-stride segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg7e64.v` | Strided segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg7e64.v` | Strided segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg7ei64.v` | Unordered indexed segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg7ei64.v` | Ordered indexed segmented load, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg7ei64.v` | Unordered indexed segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg7ei64.v` | Ordered indexed segmented store, 7 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg7e64ff.v` | Fault-only-first segmented load of 7 fields of 64-bit elements. |
| `vlseg8e64.v` | Unit-stride segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsseg8e64.v` | Unit-stride segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlsseg8e64.v` | Strided segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vssseg8e64.v` | Strided segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vluxseg8ei64.v` | Unordered indexed segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vloxseg8ei64.v` | Ordered indexed segmented load, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsuxseg8ei64.v` | Unordered indexed segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vsoxseg8ei64.v` | Ordered indexed segmented store, 8 fields, encoded width 64. Each field occupies its own EMUL-sized register group; only legal register-group combinations are architectural. |
| `vlseg8e64ff.v` | Fault-only-first segmented load of 8 fields of 64-bit elements. |
| `vl1re8.v` | Load 1 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl1re16.v` | Load 1 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl1re32.v` | Load 1 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl1re64.v` | Load 1 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs1r.v` | Store 1 whole register(s), independent of current vl. |
| `vl2re8.v` | Load 2 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl2re16.v` | Load 2 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl2re32.v` | Load 2 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl2re64.v` | Load 2 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs2r.v` | Store 2 whole register(s), independent of current vl. |
| `vl4re8.v` | Load 4 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl4re16.v` | Load 4 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl4re32.v` | Load 4 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl4re64.v` | Load 4 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs4r.v` | Store 4 whole register(s), independent of current vl. |
| `vl8re8.v` | Load 8 whole register(s), encoded element width 8; transfer size depends on VLEN rather than vl. |
| `vl8re16.v` | Load 8 whole register(s), encoded element width 16; transfer size depends on VLEN rather than vl. |
| `vl8re32.v` | Load 8 whole register(s), encoded element width 32; transfer size depends on VLEN rather than vl. |
| `vl8re64.v` | Load 8 whole register(s), encoded element width 64; transfer size depends on VLEN rather than vl. |
| `vs8r.v` | Store 8 whole register(s), independent of current vl. |
| `vlm.v` | Load ceil(vl/8) packed mask bytes. |
| `vsm.v` | Store ceil(vl/8) packed mask bytes. |

</details>

### CSR effects

Uses V CSRs; fault-only-first may change vl, ordinary element faults may change vstart. No memory-family-specific CSR.

[Back to contents](#contents)
