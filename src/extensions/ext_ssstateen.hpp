// Ssstateen's state-enable CSRs -- see ext_ssstateen.cpp.
#pragma once
#include "registers.hpp"
#include <cstdint>

namespace stateen {

constexpr uint16_t CSR_MSTATEEN0 = 0x30C; // .. 0x30F
constexpr uint16_t CSR_SSTATEEN0 = 0x10C; // .. 0x10F
constexpr uint16_t CSR_HSTATEEN0 = 0x60C; // .. 0x60F

bool is_stateen_csr(uint16_t csr);
uint64_t read_stateen(Registers &regs, uint16_t csr);
void write_stateen(Registers &regs, uint16_t csr, uint64_t value);
bool stateen_access_permitted(Registers &regs, uint16_t csr);
// Whether a denial came from the hypervisor's gate rather than the
// machine's -- cause 22 instead of cause 2.
bool stateen_denial_is_virtual(Registers &regs, uint16_t csr);

} // namespace stateen
