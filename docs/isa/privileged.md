# Privilege, address translation and protection

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

Follow a virtual address through translation and physical protection, or a trap through saved state and return. CSR readback and permission checks matter as much as instruction decoding.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [Zicsr — CSR access instructions](#zicsr--csr-access-instructions)
- [Machine privilege — traps and control](#machine-privilege--traps-and-control)
- [Supervisor privilege and Sv39](#supervisor-privilege-and-sv39)
- [Svinval — fine-grained translation invalidation](#svinval--fine-grained-translation-invalidation)
- [Svnapot — naturally aligned translation ranges](#svnapot--naturally-aligned-translation-ranges)
- [Svpbmt — page-based memory types](#svpbmt--page-based-memory-types)
- [Svade and Svadu — accessed and dirty policy](#svade-and-svadu--accessed-and-dirty-policy)
- [Smpmp — physical memory protection](#smpmp--physical-memory-protection)
- [Ssnpm — pointer masking](#ssnpm--pointer-masking)
- [Smnpm — pointer masking](#smnpm--pointer-masking)
- [Sspm — pointer masking](#sspm--pointer-masking)

## Zicsr — CSR access instructions

CSR numbers encode minimum privilege in bits 9:8 and read-only status in bits 11:10. A CSR is not an MMIO address. The executor handles ordinary storage, computed aliases, WARL writes and device-backed side effects. Unknown CSR numbers still fall back to generic storage; successful access does not prove an extension exists.

Implementation: [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `csrrw` | Read old CSR into rd and replace it with rs1. Architecturally rd=x0 suppresses the read; DoomV still computes an old value, though its implemented effective reads are side-effect-free. |
| `csrrs` | Read CSR; OR in rs1. rs1=x0 suppresses the write, but a nonzero register number holding zero still requests a write. |
| `csrrc` | Read CSR; clear bits set in rs1. rs1=x0 suppresses the write. |
| `csrrwi` | Replace CSR with zero-extended 5-bit immediate and return old value. |
| `csrrsi` | Set bits selected by 5-bit immediate; immediate zero is a pure read. |
| `csrrci` | Clear bits selected by 5-bit immediate; immediate zero is a pure read. |

### CSR effects

Zicsr defines the access instructions, **not the existence of every CSR**. Each owning extension below lists its state. `csrr`, `csrw`, `csrs` and `csrc` are assembler aliases. Reading `mtopei/stopei` is a peek; actual writes claim an interrupt.

[Back to contents](#contents)

## Machine privilege — traps and control

Reset is M mode, virtualization false, with generic CSR storage zeroed. Trap entry records PC/cause/tval, saves interrupt enable into previous-enable state, records old privilege, disables the destination global interrupt enable and selects its direct vector. ECALL does not advance epc; the handler must advance it before returning when appropriate.

The current SRET path selects `vsstatus` and `vsepc` during virtual execution,
and checks `hstatus.VTSR` for a VS-mode intercept. WFI checks `mstatus.TW`
before `hstatus.VTW`; a denied wait can therefore trap even though an allowed
wait completes immediately.

Remaining source limitations include incomplete MRET/SRET caller-privilege
checks and SRET's TSR handling. Unknown SYSTEM immediates can still advance
PC without trapping. The return paths do not visibly clear MPRV when returning
below M mode. These specific gaps should not be confused with the now-present
virtual SRET state selection and WFI intercepts.

Implementation: [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp), [src/registers.cpp](../../src/registers.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `mret` | Restore privilege from MPP, interrupt-enable from MPIE and PC from mepc; restore H virtualization from MPV when applicable. |
| `wfi` | Check TW below M mode, then VTW for VS mode. Denial raises illegal-instruction or virtual-instruction respectively. Otherwise advance PC and check interrupts again on a following step. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `mstatus` | `0x300` | Machine status: interrupt enable/previous enable, previous privilege, MPRV, SUM/MXR, TVM, FS/VS and H-related fields. Generic writable storage is not full WARL enforcement. |
| `misa` | `0x301` | Computed XLEN and I/M/A/C/F/D/V/H bits plus unconditional S/U; writes do not change configuration. |
| `medeleg` | `0x302` | Exception delegation from below M to S/HS; M-origin traps stay in M. |
| `mideleg` | `0x303` | Interrupt delegation; also masks the S-mode interrupt aliases. |
| `mie` | `0x304` | Per-cause interrupt enables. |
| `mtvec` | `0x305` | Trap base; MODE bits cleared on writes, so direct mode only. |
| `mscratch` | `0x340` | Firmware scratch storage, e.g. pointer used when swapping a register on trap entry. |
| `mepc` | `0x341` | PC saved on M-mode trap, consumed by MRET. |
| `mcause` | `0x342` | Cause number plus interrupt indicator for M trap. |
| `mtval` | `0x343` | Fault address or instruction-related trap value. |
| `mip` | `0x344` | Effective pending causes computed from timer, IMSIC and selected software shadow bits. |
| `menvcfg` | `0x30A` | Environment fields used by Sstc, PBMT and pointer masking; not every standardized field is modeled. |
| `mcounteren` | `0x306` | Lower-mode access permissions for unprivileged counters. |
| `mvendorid` | `0xF11` | Generic reset-zero read-only-by-address storage; no specific vendor identity supplied. |
| `marchid` | `0xF12` | Generic reset-zero architecture identity. |
| `mimpid` | `0xF13` | Generic reset-zero implementation identity. |
| `mhartid` | `0xF14` | Generic reset-zero value agrees with the single hart 0 platform. |

[Back to contents](#contents)

## Supervisor privilege and Sv39

S mode owns process translation and ordinary OS trap handling; U-mode application accesses are constrained by PTE permissions and PMP. Sv39 uses 3 levels of 512 eight-byte PTEs and 4-KiB pages, with 2-MiB/1-GiB aligned superpage leaves. Upper virtual bits must sign-extend bit 38. Read/write/execute, U, A and D bits are checked. SUM permits certain S accesses to U pages; MXR allows loads from executable pages. Every walk rereads guest RAM; no TLB exists. RV32 execution support does not imply Sv32 paging: the CSR/walker paths here are RV64/Sv39-shaped.

Implementation: [src/mmu.cpp](../../src/mmu.cpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `sret` | Restore supervisor interrupt state and previous privilege; use vsstatus/vsepc while virtual, otherwise machine-backed supervisor state/sepc. VS checks VTSR; HS may restore virtualization from hstatus.SPV. See remaining limitations in machine privilege. |
| `sfence.vma` | Synchronize page-table updates and local translations by virtual-address/ASID operands. No cached translation exists; DoomV checks U-mode denial and S-mode mstatus.TVM. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `sstatus` | `0x100` | Masked mstatus view. Current mask contains SIE/SPIE/SPP/SUM/MXR, not the full architectural FS/VS/SD view. |
| `sie` | `0x104` | mie intersected with mideleg; writes update delegated enables only. |
| `stvec` | `0x105` | Direct-mode S/HS trap base. |
| `scounteren` | `0x106` | U-mode counter access gate. |
| `senvcfg` | `0x10A` | Supervisor environment storage; PMM used for U-mode pointer masking. |
| `sscratch` | `0x140` | Supervisor scratch register for software trap entry/context bookkeeping. |
| `sepc` | `0x141` | Saved S/HS trap PC, consumed by SRET. |
| `scause` | `0x142` | S/HS cause and interrupt indicator. |
| `stval` | `0x143` | S/HS fault value, usually faulting virtual address. |
| `sip` | `0x144` | Computed mip masked by mideleg; restricted software writes alter shadow state. |
| `satp` | `0x180` | RV64 MODE[63:60], ASID[59:44], root PPN[43:0]; only Bare(0)/Sv39(8) writes accepted. |

[Back to contents](#contents)

## Svinval — fine-grained translation invalidation

Separate invalidations from the surrounding ordering operations. These have no cache work in DoomV but do have U-mode privilege checks.

Implementation: [src/extensions/ext_svinval.cpp](../../src/extensions/ext_svinval.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `sinval.vma` | Invalidate the named virtual-address/ASID translation; S-mode TVM causes illegal instruction. |
| `sfence.w.inval` | Order previous page-table writes before subsequent invalidations; not gated by TVM in this helper. |
| `sfence.inval.ir` | Order prior invalidations before later implicit translation reads; not gated by TVM in this helper. |

### CSR effects

No new CSR. Uses existing privilege and mstatus.TVM. H-specific HINVAL forms are not present.

[Back to contents](#contents)

## Svnapot — naturally aligned translation ranges

No new instructions. Gives PTE bit 63 (N) meaning. The implemented supported NAPOT shape is a 64-KiB mapping at the 4-KiB leaf level: virtual low VPN bits supply part of the final PPN. Reserved shapes fault instead of silently aliasing pages. This is page-table NAPOT, distinct from PMP NAPOT regions.

Implementation: [src/mmu.cpp](../../src/mmu.cpp).

### CSR effects

No new CSR; changes PTE interpretation under satp/vsatp and the runtime SVNAPOT flag.

[Back to contents](#contents)

## Svpbmt — page-based memory types

No new instructions. Leaf PTE bits 62:61 specify PMA/default(0), non-cacheable(1), or I/O(2); 3 is reserved. Nonzero PBMT requires the enabled extension and menvcfg.PBMTE. With no caches, the data-path distinction is limited, but reserved and permission checks remain observable.

Implementation: [src/mmu.cpp](../../src/mmu.cpp).

### CSR effects

Adds meaning to `menvcfg.PBMTE` bit 62 and PTE fields, not a new CSR. H environment interactions are not a complete implementation merely because henvcfg storage exists.

[Back to contents](#contents)

## Svade and Svadu — accessed and dirty policy

Svade behavior is present in the walker without an independent parser flag: a clear A bit faults, and a clear D bit faults on stores/AMOs. The walker does not update these bits. Svadu automatic updates are not implemented; MENVCFG_ADUE is declared but unused. These names distinguish the implemented policy from the alternative.

Implementation: [src/mmu.cpp](../../src/mmu.cpp).

### CSR effects

Uses PTE A[6]/D[7]. No dedicated CSR. `menvcfg.ADUE` bit 61 does not select a live update policy in this implementation.

[Back to contents](#contents)

## Smpmp — physical memory protection

Sixteen implemented entries with R/W/X, address mode A[4:3] and lock L[7]. Modes: OFF, TOR, NA4, NAPOT. TOR gets the lower bound from the prior pmpaddr, or zero for entry 0. Lowest matching entry wins. S/U with no match are denied; M bypasses unlocked permission entries but locked entries constrain M too. A locked TOR entry also locks the preceding address that forms its lower bound. The helper is RV64-shaped; odd pmpcfg registers return zero/ignore writes, rather than implementing RV32 packing or necessarily trapping.

PMP denies access with access-fault causes 1/5/7, not page-fault causes 12/13/15. Checks occur for final accesses and page-table accesses. Some vector/H access paths bypass the final wrapper, so enforcement is not uniform across all instruction families.

Implementation: [src/pmp.hpp](../../src/pmp.hpp), [src/pmp.cpp](../../src/pmp.cpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp).

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `pmpcfg0` | `0x3A0` | Eight packed permission/config bytes for entries 0–7. |
| `pmpcfg2` | `0x3A2` | Eight packed bytes for entries 8–15. |
| `pmpaddr0` | `0x3B0` | Entry 0 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr1` | `0x3B1` | Entry 1 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr2` | `0x3B2` | Entry 2 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr3` | `0x3B3` | Entry 3 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr4` | `0x3B4` | Entry 4 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr5` | `0x3B5` | Entry 5 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr6` | `0x3B6` | Entry 6 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr7` | `0x3B7` | Entry 7 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr8` | `0x3B8` | Entry 8 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr9` | `0x3B9` | Entry 9 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr10` | `0x3BA` | Entry 10 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr11` | `0x3BB` | Entry 11 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr12` | `0x3BC` | Entry 12 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr13` | `0x3BD` | Entry 13 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr14` | `0x3BE` | Entry 14 address encoded in units of four bytes; 54 writable address bits. |
| `pmpaddr15` | `0x3BF` | Entry 15 address encoded in units of four bytes; 54 writable address bits. |

Unimplemented pmpaddr16–63 read zero and ignore writes; higher configuration fields remain zero. This snapshot has no Smepmp mseccfg policy implementation.

[Back to contents](#contents)

## Ssnpm — pointer masking

Supervisor control of pointer masking for lower execution contexts. The single SSNPM flag enables the shared code. PMM values 0/2/3 select PMLEN 0/7/16; reserved value 1 preserves the old legal field on write. Ordinary S accesses select menvcfg; U accesses select senvcfg; HLV/HSV use hstatus.HUPMM. Data addresses have ignored high bits reconstructed by sign extension of the remaining address. Fetch and page-table-generated addresses are not ordinary tagged data pointers. M-mode masking (Smmpm) is not implemented.

Implementation: [src/mmu.cpp](../../src/mmu.cpp), [src/extensions/ext_h.cpp](../../src/extensions/ext_h.cpp), [src/extensions.cpp](../../src/extensions.cpp).

### CSR effects

No new CSR. Uses `menvcfg/senvcfg/henvcfg.PMM[33:32]` storage and `hstatus.HUPMM[49:48]`; not every virtual PMM selection is implemented.

[Back to contents](#contents)

## Smnpm — pointer masking

Machine control of pointer masking for lower execution contexts. The single SSNPM flag enables the shared code. PMM values 0/2/3 select PMLEN 0/7/16; reserved value 1 preserves the old legal field on write. Ordinary S accesses select menvcfg; U accesses select senvcfg; HLV/HSV use hstatus.HUPMM. Data addresses have ignored high bits reconstructed by sign extension of the remaining address. Fetch and page-table-generated addresses are not ordinary tagged data pointers. M-mode masking (Smmpm) is not implemented.

Implementation: [src/mmu.cpp](../../src/mmu.cpp), [src/extensions/ext_h.cpp](../../src/extensions/ext_h.cpp), [src/extensions.cpp](../../src/extensions.cpp).

### CSR effects

No new CSR. Uses `menvcfg/senvcfg/henvcfg.PMM[33:32]` storage and `hstatus.HUPMM[49:48]`; not every virtual PMM selection is implemented.

[Back to contents](#contents)

## Sspm — pointer masking

Repository spelling/alias accepted by its parser for the shared masking mechanism; do not assume every external tool recognizes this token. The single SSNPM flag enables the shared code. PMM values 0/2/3 select PMLEN 0/7/16; reserved value 1 preserves the old legal field on write. Ordinary S accesses select menvcfg; U accesses select senvcfg; HLV/HSV use hstatus.HUPMM. Data addresses have ignored high bits reconstructed by sign extension of the remaining address. Fetch and page-table-generated addresses are not ordinary tagged data pointers. M-mode masking (Smmpm) is not implemented.

Implementation: [src/mmu.cpp](../../src/mmu.cpp), [src/extensions/ext_h.cpp](../../src/extensions/ext_h.cpp), [src/extensions.cpp](../../src/extensions.cpp).

### CSR effects

No new CSR. Uses `menvcfg/senvcfg/henvcfg.PMM[33:32]` storage and `hstatus.HUPMM[49:48]`; not every virtual PMM selection is implemented.

[Back to contents](#contents)
