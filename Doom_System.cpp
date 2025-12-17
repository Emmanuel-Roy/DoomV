#include "Doom_System.hpp"
#include <cstdio>
#include <algorithm>
#include <cstring>

Doom_System::Doom_System() : cpu(this), ram(RAM_SIZE, 0) {
    // Initialize memory to avoid startup artifacts
    std::memset(framebuffer, 0, sizeof(framebuffer));
    std::memset(palette, 0, sizeof(palette));
    
    init_font();
    
    // Initial test instruction: LUI x1, 0x12345
    write32(RAM_BASE, 0x123450B7);
}

Doom_System::~Doom_System() {
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}

bool Doom_System::init_sdl(int scale_factor) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;

    int window_w = TOTAL_W * scale_factor;
    int window_h = TOTAL_H * scale_factor;

    window = SDL_CreateWindow("RISC-V Doom SoC", 
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                              window_w, window_h, SDL_WINDOW_SHOWN);

    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, TOTAL_W, TOTAL_H);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, 
                                SDL_TEXTUREACCESS_STREAMING, TOTAL_W, TOTAL_H);

    return (renderer && texture);
}

// --- BUS LOGIC ---

uint8_t Doom_System::read8(uint32_t addr) {
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) 
        return ram[addr - RAM_BASE];
    if (addr >= MMIO_FB && addr < MMIO_FB + (BASE_W * BASE_H)) 
        return framebuffer[addr - MMIO_FB];
    if (addr >= MMIO_PAL && addr < MMIO_PAL + 768) 
        return palette[addr - MMIO_PAL];
    return 0;
}

uint16_t Doom_System::read16(uint32_t addr) {
    return (uint16_t)read8(addr) | ((uint16_t)read8(addr + 1) << 8);
}

uint32_t Doom_System::read32(uint32_t addr) {
    if (addr == MMIO_INPUT) return key_register;
    if (addr == 0x10000004) return (uint32_t)SDL_GetTicks();
    return (uint32_t)read16(addr) | ((uint32_t)read16(addr + 2) << 16);
}

void Doom_System::write8(uint32_t addr, uint8_t val) {
    // 1. RAM Access
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) {
        ram[addr - RAM_BASE] = val;
    }
    // 2. Framebuffer Access (VRAM)
    else if (addr >= MMIO_FB && addr < MMIO_FB + (BASE_W * BASE_H)) {
        framebuffer[addr - MMIO_FB] = val;
        vram_updates++; // Track pixel writes for FPS calculation
    }
    // 3. Palette Access
    else if (addr >= MMIO_PAL && addr < MMIO_PAL + 768) {
        palette[addr - MMIO_PAL] = val;
    }
}

void Doom_System::write32(uint32_t addr, uint32_t val) {
    // RISC-V uses Little Endian: 
    // The least significant byte goes to the lowest address.
    write8(addr + 0, (val >> 0)  & 0xFF);
    write8(addr + 1, (val >> 8)  & 0xFF);
    write8(addr + 2, (val >> 16) & 0xFF);
    write8(addr + 3, (val >> 24) & 0xFF);
}

// --- DECOUPLED RENDERING ---

void Doom_System::update_display() {
    static uint32_t screen[TOTAL_W * TOTAL_H];
    
    // Background: #876A96
    std::fill(screen, screen + (TOTAL_W * TOTAL_H), 0x876A96); 

    // --- YOUR PALETTE ---
    uint32_t pal_pink   = 0xD580B8; // Headers (consistent 3-dash style)
    uint32_t pal_white  = 0xD7D0D0; // Main Data (Register Values, CURR PC)
    uint32_t pal_red    = 0xB95167; // Register Labels (X00)
    uint32_t pal_dark   = 0x25080C; // Shadow
    uint32_t pal_stats  = 0xC3A9C4; // Performance Metrics & Trace History

    // 1. GAME SCREEN (400x250)
    int tx = 15, ty = 55;
    for (int y = 0; y < 250; y++) {
        for (int x = 0; x < 400; x++) {
            int sx = (x * BASE_W) / 400;
            int sy = (y * BASE_H) / 250;
            uint8_t idx = framebuffer[sy * BASE_W + sx];
            uint32_t r = palette[idx*3], g = palette[idx*3+1], b = palette[idx*3+2];
            screen[(y+ty)*TOTAL_W+(x+tx)] = (r << 16) | (g << 8) | b;
        }
    }

    char buf[64];
    int hud_x = 425;

    auto draw_shadow_text = [&](int x, int y, const char* s, uint32_t col) {
        draw_string(screen, x + 1, y + 1, s, pal_dark); 
        draw_string(screen, x, y, s, col);
    };

    // --- SECTION 1: STATS ---
    int current_y = 10;
    draw_shadow_text(hud_x, current_y, "--- STATS ---", pal_pink);
    current_y += 15;
    
    sprintf(buf, "DASH FPS: %.1f", dashboard_fps);
    draw_shadow_text(hud_x, current_y, buf, pal_stats);
    current_y += 12;
    sprintf(buf, "DOOM FPS: %.1f", doom_fps);
    draw_shadow_text(hud_x, current_y, buf, pal_stats);
    current_y += 12;
    sprintf(buf, "LATENCY : %.2f ns/i", avg_ns_per_instr);
    draw_shadow_text(hud_x, current_y, buf, pal_stats);

    // GAP: 20 pixels
    current_y += 20;

    // --- SECTION 2: REGISTER FILE ---
    draw_shadow_text(hud_x, current_y, "--- REGISTER FILE ---", pal_pink);
    int reg_start_y = current_y + 15;
    for (int i = 0; i < 32; i++) {
        int cx = (i < 16) ? hud_x : hud_x + 105;
        int cy = reg_start_y + ((i % 16) * 11);
        
        sprintf(buf, "X%02d:", i);
        draw_shadow_text(cx, cy, buf, pal_red);
        
        sprintf(buf, "%08X", cpu.regs[i]);
        draw_shadow_text(cx + 35, cy, buf, pal_white);
    }

    // GAP: 20 pixels
    current_y = reg_start_y + (16 * 11) + 20;

    // --- SECTION 3: TRACE LOG ---
    draw_shadow_text(hud_x, current_y, "--- TRACE LOG ---", pal_pink);
    current_y += 15;
    
    // CURR PC in White
    sprintf(buf, "CURR PC: %08X", cpu.pc);
    draw_shadow_text(hud_x, current_y, buf, pal_white);
    
    // History in Stats color
    for (int i = 0; i < 4; i++) {
        auto& h = cpu.history[(cpu.history_ptr + i) % 5];
        sprintf(buf, "%08X: %08X", h.addr, h.val);
        draw_shadow_text(hud_x, (current_y + 15) + (i * 11), buf, pal_stats);
    }

    SDL_UpdateTexture(texture, nullptr, screen, TOTAL_W * 4);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

void Doom_System::draw_char(uint32_t* p, int x, int y, char c, uint32_t col) {
    if ((uint8_t)c >= 128) return;
    for (int r = 0; r < 8; r++) {
        uint8_t b = font8x8[(uint8_t)c][r];
        for (int cl = 0; cl < 8; cl++) {
            if (b & (0x80 >> cl)) {
                int tx = x + cl, ty = y + r;
                if (tx >= 0 && tx < TOTAL_W && ty >= 0 && ty < TOTAL_H)
                    p[ty * TOTAL_W + tx] = col;
            }
        }
    }
}


void Doom_System::draw_string(uint32_t* p, int x, int y, const char* s, uint32_t c) {
    while (*s) { draw_char(p, x, y, *s++, c); x += 8; }
}

void Doom_System::handle_input() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_KEYDOWN) key_register = e.key.keysym.sym;
        if (e.type == SDL_KEYUP) key_register = 0;
    }
}

void Doom_System::run() {
    uint32_t last_sync = SDL_GetTicks();
    
    while (true) {
        // CPU Burst
        for (int i = 0; i < 20000; i++) cpu.step();

        uint32_t now = SDL_GetTicks();
        uint32_t delta = now - last_sync;

        if (delta >= 16) { // ~60 FPS dashboard refresh
            // Dashboard FPS: How fast the SDL window is drawing
            dashboard_fps = 1000.0f / (float)delta;

            // Doom FPS: (Total pixel writes / pixels per frame) / (time in seconds)
            // This tells you how many full frames the RISC-V CPU drew
            float total_pixels = (float)(BASE_W * BASE_H);
            doom_fps = ((float)vram_updates / total_pixels) / ((float)delta / 1000.0f);
            
            vram_updates = 0; // Reset counter for next second
            
            handle_input();
            update_display();
            last_sync = now;
        }
    }
}

void Doom_System::init_font() {
    std::memset(font8x8, 0, sizeof(font8x8));
    auto set_char = [this](char c, std::initializer_list<uint8_t> rows) {
        int i = 0;
        for (uint8_t row : rows) {
            if (i < 8) font8x8[(uint8_t)c][i++] = row;
        }
    };

    // --- Numbers & Hex ---
    set_char('0', {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00});
    set_char('1', {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00});
    set_char('2', {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00});
    set_char('3', {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00});
    set_char('4', {0x0C, 0x1C, 0x2C, 0x4C, 0x7E, 0x0C, 0x0C, 0x00});
    set_char('5', {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00});
    set_char('6', {0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00});
    set_char('7', {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00});
    set_char('8', {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00});
    set_char('9', {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x66, 0x3C, 0x00});
    set_char('A', {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00});
    set_char('B', {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00});
    set_char('C', {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00});
    set_char('D', {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00});
    set_char('E', {0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x7E, 0x00});
    set_char('F', {0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x60, 0x00});

    // --- Performance Text (LATENCY, DASH, FPS, DOOM) ---
    set_char('G', {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00});
    set_char('H', {0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00});
    set_char('I', {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00});
    set_char('L', {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00});
    set_char('M', {0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00});
    set_char('N', {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00});
    set_char('O', {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00});
    set_char('P', {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00});
    set_char('R', {0x7C, 0x66, 0x66, 0x7C, 0x78, 0x6C, 0x66, 0x00});
    set_char('S', {0x3E, 0x60, 0x60, 0x3C, 0x06, 0x06, 0x7C, 0x00});
    set_char('T', {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00});
    set_char('U', {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00});
    set_char('V', {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00});
    set_char('Y', {0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00});

    // --- Units & Symbols (ns/ins, '.', '/') ---
    set_char('n', {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00});
    set_char('s', {0x00, 0x00, 0x3C, 0x60, 0x3C, 0x06, 0x3C, 0x00});
    set_char('i', {0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00});
    set_char('.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00});
    set_char('/', {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00});
    set_char(':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00});
    set_char('-', {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00});
    set_char(' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}