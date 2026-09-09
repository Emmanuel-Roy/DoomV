# Hypervisor state and guest translation

[Documentation home](../README.md) · [ISA guide](../ISA_EXTENSIONS.md)

Keep privilege level and virtualization state separate. Explicit guest-memory instructions and ordinary execution do not necessarily follow the same implementation path.

Inventory baseline: `6b37ec0`. Selected behavior corrections reviewed at `c7d881b`; see the [evidence notes](../README.md#reading-the-evidence).

## Contents

- [H — hypervisor extension](#h--hypervisor-extension)

## H — hypervisor extension

H is off by default. Privilege and virtualization are separate: M, HS=(S,V=0), VS=(S,V=1), host U and VU. First-stage guest translation maps GVA→GPA via vsatp; G-stage maps GPA→physical via hgatp. Even the guest page-table accesses themselves need G-stage translation. Guest-page faults use causes 20/21/23, while virtual-instruction faults use 22.

The explicit HLV/HSV helper checks virtual callers and HU, then calls
`mmu_translate(as_guest=true)` directly. Ordinary fetch/load/store calls leave
`as_guest=false`, and the walker does not inspect `get_virt`. Ordinary VS/VU
execution therefore does not automatically take the explicit guest two-stage
path. HLV/HSV also bypass the final `translate_or_trap` wrapper.

At `c7d881b`, SRET selects guest status/PC while virtual and checks VTSR.
WFI checks TW and VTW, and the CSR/SFENCE paths contain VTVM checks. These are
implemented intercepts, not merely writable control bits. They do not resolve
the broader translation, interrupt-injection, guest-time and width-specific
coverage limits. Hypervisor conformance still needs independent tests.

Implementation: [src/extensions/ext_h.cpp](../../src/extensions/ext_h.cpp), [src/extensions/ext_h_ldst.cpp](../../src/extensions/ext_h_ldst.cpp), [src/extensions/ext_zicsr.cpp](../../src/extensions/ext_zicsr.cpp), [src/mmu.cpp](../../src/mmu.cpp).

| Instruction | Operation and relevant details |
|---|---|
| `hlv.b` | Load 8 bits using guest translation; narrow signed forms sign-extend. |
| `hlv.bu` | Load and zero-extend 8 guest bits. |
| `hsv.b` | Store low 8 bits through guest translation. |
| `hlv.h` | Load 16 bits using guest translation; narrow signed forms sign-extend. |
| `hlv.hu` | Load and zero-extend 16 guest bits. |
| `hsv.h` | Store low 16 bits through guest translation. |
| `hlv.w` | Load 32 bits using guest translation; narrow signed forms sign-extend. |
| `hlv.wu` | Load and zero-extend 32 guest bits. |
| `hsv.w` | Store low 32 bits through guest translation. |
| `hlv.d` | Load 64 bits using guest translation; narrow signed forms sign-extend. |
| `hsv.d` | Store low 64 bits through guest translation. |
| `hlvx.hu` | Load and zero-extend 16 guest bits using execute permission. |
| `hlvx.wu` | Load and zero-extend 32 guest bits using execute permission. |
| `hfence.vvma` | Synchronize guest first-stage translations; current helper immediately advances PC before privilege validation. |
| `hfence.gvma` | Synchronize G-stage translations by guest physical address/VMID; no cached translation exists, and the same early-return limitation applies. |

### CSR effects

| CSR | Address | Purpose and DoomV behavior |
|---|---|---|
| `hstatus` | `0x600` | HS virtualization status: SPV/SPVP record guest origin, HU permits host U guest-memory instructions; GVA and HUPMM used. VSXL hardwired 64, VSBE/VGEIN zero. VTVM/VTW/VTSR storage is not proof all intercepts work. |
| `hedeleg` | `0x602` | Exception delegation from HS to VS, after M delegation; reserved/HS-only exception bits masked. |
| `hideleg` | `0x603` | Guest interrupt delegation with writable-mask filtering. |
| `hie` | `0x604` | Hypervisor interrupt-enable architectural role; generic storage does not implement every hie/mie alias. |
| `htimedelta` | `0x605` | Architectural guest time offset; current counter helper does not add it to time. |
| `hcounteren` | `0x606` | Architectural guest counter gate; used in scountovf filtering, but ordinary counter_permitted omits this gate. |
| `hgeie` | `0x607` | Guest external-interrupt enable role; no guest IMSIC files are modeled. |
| `henvcfg` | `0x60A` | Virtual environment controls; generic fields plus PMM WARL handling, not full field semantics. |
| `htval` | `0x643` | Guest physical fault information, normally GPA >> 2, separate from stval virtual address. |
| `hip` | `0x644` | Architectural guest interrupt pending view; full alias/interrupt-injection behavior is not implemented. |
| `hvip` | `0x645` | Architectural virtual interrupt injection; no comprehensive connection to compute_mip. |
| `htinst` | `0x64A` | Architectural transformed faulting instruction role; no full transformed-instruction implementation. |
| `hgatp` | `0x680` | Second-stage translation root; Bare or Sv39x4. Sv39x4 has a 16-KiB root and 11-bit top index. |
| `hgeip` | `0xE12` | Guest external-interrupt pending role; GEILEN=0, no guest-file delivery. |
| `vsstatus` | `0x200` | Guest supervisor status; S-name CSR instructions redirect here when virtual. |
| `vsie` | `0x204` | Guest interrupt-enable state; S-name CSR instructions redirect here when virtual. |
| `vstvec` | `0x205` | Guest direct-mode trap vector; S-name CSR instructions redirect here when virtual. |
| `vsscratch` | `0x240` | Guest supervisor scratch; S-name CSR instructions redirect here when virtual. |
| `vsepc` | `0x241` | Guest saved trap PC; S-name CSR instructions redirect here when virtual. |
| `vscause` | `0x242` | Guest trap cause; S-name CSR instructions redirect here when virtual. |
| `vstval` | `0x243` | Guest fault value; S-name CSR instructions redirect here when virtual. |
| `vsip` | `0x244` | Guest pending-interrupt state; S-name CSR instructions redirect here when virtual. |
| `vsatp` | `0x280` | Guest first-stage page-table root, Bare/Sv39 WARL; S-name CSR instructions redirect here when virtual. |

H also adds `mstatus.MPV[39]` and `GVA[38]`. RV32 high-half virtualization CSRs are not modeled as a complete RV32 H implementation.

[Back to contents](#contents)
