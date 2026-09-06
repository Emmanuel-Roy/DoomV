#pragma once
#include "registers.hpp"
#include <cstdint>

// mstatus's extension-state fields: FS (bits 14:13) for floating point and
// VS (bits 10:9) for vector. Both work the same way -- Off(0) / Initial(1) /
// Clean(2) / Dirty(3) -- and both are checked in the same place in the
// decoder, so they live together here rather than inside either extension's
// own header.
//
// While a field reads Off, every instruction belonging to that unit is an
// illegal instruction: software has to turn the unit on before using it.
// DoomV ignored this for a long time, which is how the V differential test
// came to be written wrong -- it ran fine here and trapped on its first
// vsetivli under spike, because spike enforces what real hardware does.
//
// The Dirty half matters as much as the trap. A supervisor decides whether a
// task has live FP or vector state worth saving on a context switch by
// checking whether the field reads Dirty. A hart that never sets it lets the
// kernel skip saving registers that really are live, so state leaks silently
// between tasks and surfaces much later as corruption with no obvious cause.
//
// Note that firmware usually turns both on before any OS runs: OpenSBI sets
// FS when misa reports F or D, and VS when it reports V (lib/sbi/sbi_hart.c).
// So enforcing this changes nothing for a normal Linux boot -- it only
// catches a bare-metal guest that never enabled the unit it is using.
namespace vcommon {

constexpr uint16_t CSR_MSTATUS_X = 0x300;

constexpr uint64_t MSTATUS_FS_MASK  = 3ull << 13;
constexpr uint64_t MSTATUS_FS_DIRTY = 3ull << 13;
constexpr uint64_t MSTATUS_VS_MASK  = 3ull << 9;
constexpr uint64_t MSTATUS_VS_DIRTY = 3ull << 9;

inline bool fp_unit_enabled(Registers &regs)
{
	return (regs.read_csr(CSR_MSTATUS_X) & MSTATUS_FS_MASK) != 0;
}

inline bool vector_unit_enabled(Registers &regs)
{
	return (regs.read_csr(CSR_MSTATUS_X) & MSTATUS_VS_MASK) != 0;
}

inline void mark_fp_dirty(Registers &regs)
{
	regs.write_csr(CSR_MSTATUS_X, regs.read_csr(CSR_MSTATUS_X) | MSTATUS_FS_DIRTY);
}

inline void mark_vector_dirty(Registers &regs)
{
	regs.write_csr(CSR_MSTATUS_X, regs.read_csr(CSR_MSTATUS_X) | MSTATUS_VS_DIRTY);
}

} // namespace vcommon
