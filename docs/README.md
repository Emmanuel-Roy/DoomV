# Understanding DoomV

DoomV interprets RISC-V instructions in a C++ program. The same simulated
machine can run a bare-metal DOOM guest or boot OpenSBI, Linux and BusyBox.
These guides explain the code that connects those pieces.

## Choose a starting point

| You want to understand… | Start here | Then read |
|---|---|---|
| How a guest reaches its first useful output | [Boot walkthrough](BOOT_FLOW.md) | Console and display in the architecture guide |
| Where a load, store or interrupt goes | [Devices and architecture](DEVICES_AND_ARCHITECTURE.md) | The translation example, then the address map |
| A particular instruction or CSR | [ISA reference](ISA_EXTENSIONS.md) | The relevant family, then its linked implementation |
| How to run the scripts | [Script usage](../scripts/README.md) | The actual script for its current defaults |
| Why an earlier implementation was wrong | [Bug history](BUGS.md) | Current source and tests before assuming the bug still exists |

For a first read, follow **boot → architecture → ISA**. The instruction tables
are lookup material; you do not need to read them before understanding the boot.

## Terms used throughout

| Term | Meaning in this project |
|---|---|
| Host | The Windows C++ process, its memory allocations and SDL window |
| Guest | The RISC-V program whose instructions DoomV interprets |
| Hart | One architectural execution context; DoomV models one |
| ISA | Instruction-set architecture: the contract visible to guest software |
| CSR | Control/status register, accessed by CSR instructions rather than ordinary memory loads |
| MMIO | Memory-mapped I/O: an address access handled by a device instead of RAM |
| PTE | Page-table entry: translation information and access permissions |
| PMP | Physical memory protection, checked independently of page-table permissions |
| SBI | Supervisor Binary Interface: firmware services called by the supervisor |
| DTB / FDT | Binary device tree describing the machine to firmware and Linux |
| Initramfs | An archive the kernel unpacks to provide its initial filesystem |
| Signature | Guest-produced result bytes collected and compared by a test harness |

## Reading the evidence

The original detailed inventory used commit
`6b37ec0675cb052e4821c227a7b4c57af89b280f`. This readability pass checked the
boot entry points, extension parser, trap returns, address translation and
console paths against `c7d881b`, the revision preceding these documentation edits.

Implementation links describe DoomV. Specification links explain the
architectural contract. A test result is a third kind of evidence: a source
review or a successful script syntax check does not establish that Linux boots
or that every instruction conforms. This pass did not run the emulator suites.

The guides retain limitations where the code still has them. Historical
inventory sections have not all been revalidated instruction by instruction;
the revision notes in each guide identify what this pass checked.
