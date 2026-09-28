// Sscofpmf's counter-overflow plumbing -- see ext_sscofpmf.cpp.
#pragma once
#include "registers.hpp"
#include <cstdint>

namespace sscofpmf {

constexpr uint16_t CSR_MHPMEVENT3  = 0x323;
constexpr uint16_t CSR_MHPMEVENT31 = 0x33F;
constexpr uint16_t CSR_SCOUNTOVF   = 0xDA0;
constexpr uint16_t CSR_MCOUNTEREN  = 0x306;
constexpr uint16_t CSR_HCOUNTEREN  = 0x606;

// Local counter-overflow interrupt. Bit 13 of mip/mie, and the reason
// Sscofpmf needs interrupt plumbing at all rather than just CSRs.
constexpr uint64_t MIP_LCOFI = 1ull << 13;

bool is_mhpmevent(uint16_t csr);
uint64_t read_scountovf(Registers &regs);
uint64_t mhpmevent_wmask();
bool lcofi_pending(Registers &regs);

} // namespace sscofpmf
