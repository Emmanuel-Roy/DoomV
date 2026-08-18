#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <cstdint>
#include "RV32IMAC_Core.hpp"

class Doom_System {
public:
    // --- Display Constants for 16:9 Dashboard ---
    static constexpr int TOTAL_W = 640;
    static constexpr int TOTAL_H = 360;
    static constexpr int BASE_W  = 320; // Doom Internal Width
    static constexpr int BASE_H  = 200; // Doom Internal Height

    // --- The Memory Map (Your Bus Addresses) ---
    static constexpr uint32_t RAM_BASE   = 0x80000000;
    static constexpr uint32_t RAM_SIZE   = 128 * 1024 * 1024;
    static constexpr uint32_t MMIO_INPUT = 0x10000000;
    static constexpr uint32_t MMIO_FB    = 0x10001000;
    static constexpr uint32_t MMIO_PAL   = 0x10002000;

    Doom_System();
    ~Doom_System();

    bool init_sdl(int scale_factor);
    void run();

    // Bus Interface
    uint8_t  read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);
    void     write8(uint32_t addr, uint8_t val);
    void     write32(uint32_t addr, uint32_t val);

    // Friend class allows the CPU to see these private members
    friend class RV32IMAC_Core;

private:
    // The CPU is now a direct object
    RV32IMAC_Core cpu; 

    // Hardware Memory
    std::vector<uint8_t> ram;
    uint8_t framebuffer[BASE_W * BASE_H];
    uint8_t palette[256 * 3];

    // MMIO State
    uint32_t key_register;
    uint32_t timer_register;

    // Software Font Unit
    uint8_t font8x8[128][8]; 
    void init_font();

    // Host Rendering Unit
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;

    //Performance
    float avg_ns_per_instr = 0.0f;
    float dashboard_fps = 0.0f;
    float doom_fps = 0.0f;;
    float vram_updates = 0.0f;

    void draw_char(uint32_t* pixels, int x, int y, char c, uint32_t color);
    void draw_string(uint32_t* pixels, int x, int y, const char* str, uint32_t color);
    void draw_rect(uint32_t* p, int x, int y, int w, int h, uint32_t col);
    void update_display();
    void handle_input();
};