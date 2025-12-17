#pragma once
#include <cstdint>

class Doom_System; // Forward declaration

struct HistoryEntry { 
    uint32_t addr; 
    uint32_t val; 
};

class RV32IMAC_Core {
public:
    explicit RV32IMAC_Core(Doom_System* sys);
    void step();

    uint32_t regs[32];
    uint32_t pc;
    HistoryEntry history[5];
    int history_ptr = 0;

private:
    Doom_System* system;
    void execute_32(uint32_t instr);
    void execute_16(uint16_t instr);
};