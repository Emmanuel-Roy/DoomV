// Zicntr and Zihpm: the unprivileged performance counters, and the
// mcounteren/scounteren gating that decides who may read them.
//
// These two extensions add no instructions -- only CSRs -- so unlike every
// other file in this directory they have no decode_*/exec_* pair. What they
// contribute is a read value and an access rule, both consumed by
// ext_zicsr.cpp's CSR dispatch, which is the only place CSR numbers are
// interpreted.
#pragma once
#include "registers.hpp"
#include "memory.hpp"
#include <cstdint>

namespace counters {

// Zicntr: the three counters every RVA23 hart must provide.
constexpr uint16_t CSR_CYCLE   = 0xC00;
constexpr uint16_t CSR_TIME    = 0xC01;
constexpr uint16_t CSR_INSTRET = 0xC02;
// Zihpm: hpmcounter3..hpmcounter31, the programmable ones.
constexpr uint16_t CSR_HPM_FIRST = 0xC03;
constexpr uint16_t CSR_HPM_LAST  = 0xC1F;

constexpr uint16_t CSR_MCOUNTEREN = 0x306;
constexpr uint16_t CSR_SCOUNTEREN = 0x106;
// The hypervisor's gate over its guest. There is no vscounteren: the H
// extension leaves scounteren as a single register, context-switched by
// the hypervisor, so a VU-mode check reads CSR_SCOUNTEREN above.
constexpr uint16_t CSR_HCOUNTEREN = 0x606;

// True for the whole unprivileged counter window, hpm counters included.
inline bool is_counter_csr(uint16_t csr)
{
	return csr >= CSR_CYCLE && csr <= CSR_HPM_LAST;
}

// Which bit of mcounteren/scounteren guards this counter: bit 0 is cycle,
// bit 1 time, bit 2 instret, bit n hpmcounter-n.
inline int counter_index(uint16_t csr) { return (int)(csr - CSR_CYCLE); }

uint64_t read_counter(Registers &regs, Memory &mem, uint16_t csr);
bool counter_permitted(Registers &regs, uint16_t csr);
// Whether a refusal came from the hypervisor's gate rather than the
// machine's or the guest's own -- cause 22 instead of cause 2.
bool counter_denial_is_virtual(Registers &regs, uint16_t csr);

} // namespace counters
