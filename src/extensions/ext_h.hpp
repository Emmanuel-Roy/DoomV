// Declarations for the hypervisor extension's CSR plumbing (ext_h.cpp).
// Kept separate from riscv_core.hpp because these are consumed by
// ext_zicsr.cpp's dispatch rather than being an exec_* entry point of their
// own -- the H extension's first increment adds no instructions, only CSRs
// and the rules that govern reaching them.
#pragma once
#include "registers.hpp"
#include <cstdint>

namespace hyp {

bool is_hypervisor_csr(uint16_t csr);
bool is_vs_csr(uint16_t csr);
uint16_t redirect_for_virt(Registers &regs, uint16_t csr);
bool is_virtual_instruction_csr(Registers &regs, uint16_t csr);
uint64_t read_hstatus(Registers &regs);
void write_hstatus(Registers &regs, uint64_t value);

constexpr uint16_t CSR_HSTATUS_ADDR = 0x600;
constexpr uint16_t CSR_VSSTATUS_ADDR = 0x200;
constexpr uint16_t CSR_VSEPC_ADDR    = 0x241;
constexpr uint64_t CAUSE_VIRTUAL_INSTRUCTION = 22;

} // namespace hyp
