#include "RV32IMAC_Core.hpp"
#include "Doom_System.hpp"

RV32IMAC_Core::RV32IMAC_Core(Doom_System* sys) : system(sys) {
    pc = 0x80000000;
    for (int i = 0; i < 32; i++) regs[i] = 0;
    for (int i = 0; i < 5; i++) history[i] = {0, 0};
}

void RV32IMAC_Core::step() {
    regs[0] = 0; // Hardwired zero
    uint32_t current_pc = pc;
    uint16_t h1 = system->read16(pc);
    uint32_t full_instr;

    if ((h1 & 0x03) != 0x03) { // Compressed 16-bit
        full_instr = h1;
        execute_16(h1);
        if (pc == current_pc) pc += 2; // Only increment if instruction didn't jump
    } else { // Standard 32-bit
        full_instr = h1 | (system->read16(pc + 2) << 16);
        execute_32(full_instr);
        if (pc == current_pc) pc += 4; // Only increment if instruction didn't jump
    }

    // Update Dashboard History
    history[history_ptr] = {current_pc, full_instr};
    history_ptr = (history_ptr + 1) % 5;
}

void RV32IMAC_Core::execute_32(uint32_t instr) {
    uint8_t opcode = instr & 0x7F;
    uint8_t rd     = (instr >> 7) & 0x1F;
    uint8_t funct3 = (instr >> 12) & 0x07;
    uint8_t rs1    = (instr >> 15) & 0x1F;
    uint8_t rs2    = (instr >> 20) & 0x1F;

    return;
}

void RV32IMAC_Core::execute_16(uint16_t instr) { 
    return; 
}
