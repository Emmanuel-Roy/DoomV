#include "gui.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "debugger.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

Gui::Gui() : last_sync(0)
{
	init_font();
}

Gui::~Gui()
{
	if (texture) SDL_DestroyTexture(texture);
	if (renderer) SDL_DestroyRenderer(renderer);
	if (window) SDL_DestroyWindow(window);
	SDL_Quit();
}

bool Gui::init(int scale_factor)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;

	int window_w = TOTAL_W * scale_factor;
	int window_h = TOTAL_H * scale_factor;

	window = SDL_CreateWindow("RISC-V Doom SoC",
	                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                          window_w, window_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!window) return false;

	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_RenderSetLogicalSize(renderer, TOTAL_W, TOTAL_H);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
	                            SDL_TEXTUREACCESS_STREAMING, TOTAL_W, TOTAL_H);

	last_sync = SDL_GetTicks();

	return (renderer && texture);
}

void Gui::render(const Registers &regs, Memory &mem, const Debugger &dbg)
{
	uint32_t now = SDL_GetTicks();
	uint32_t delta = now - last_sync;
	if (delta == 0) delta = 1;

	dashboard_fps = 1000.0f / (float)delta;
	last_sync = now;

	static uint32_t screen[TOTAL_W * TOTAL_H];
	std::fill(screen, screen + (TOTAL_W * TOTAL_H), 0x876A96);

	uint32_t pal_pink  = 0xD580B8;
	uint32_t pal_white = 0xD7D0D0;
	uint32_t pal_red   = 0xB95167;
	uint32_t pal_dark  = 0x25080C;
	uint32_t pal_stats = 0xC3A9C4;

	// GAME SCREEN (400x250), scaled from the native 320x200 32bpp framebuffer.
	// Only 400 distinct sx values and 250 distinct sy values exist -- precompute
	// them once instead of a multiply+divide per pixel (100,000/frame otherwise).
	static int scale_x[400];
	static int scale_y[250];
	static bool scale_tables_built = false;
	if (!scale_tables_built) {
		for (int x = 0; x < 400; x++) scale_x[x] = (x * Memory::FB_W) / 400;
		for (int y = 0; y < 250; y++) scale_y[y] = (y * Memory::FB_H) / 250;
		scale_tables_built = true;
	}

	const uint32_t *fb32 = reinterpret_cast<const uint32_t *>(mem.framebuffer());
	int tx = 15, ty = 55;
	for (int y = 0; y < 250; y++) {
		const uint32_t *src_row = fb32 + scale_y[y] * Memory::FB_W;
		uint32_t *dst_row = screen + (y + ty) * TOTAL_W + tx;
		for (int x = 0; x < 400; x++) {
			dst_row[x] = src_row[scale_x[x]];
		}
	}

	char buf[64];
	int hud_x = 425;

	auto draw_shadow_text = [&](int x, int y, const char *s, uint32_t col) {
		draw_string(screen, x + 1, y + 1, s, pal_dark);
		draw_string(screen, x, y, s, col);
	};

	// STATS
	int current_y = 10;
	draw_shadow_text(hud_x, current_y, "--- STATS ---", pal_pink);
	current_y += 15;

	sprintf(buf, "DASH FPS: %.1f", dashboard_fps);
	draw_shadow_text(hud_x, current_y, buf, pal_stats);
	if (dbg.halted) {
		current_y += 12;
		draw_shadow_text(hud_x, current_y, "HALTED", pal_red);
	}

	current_y += 12;

	// REGISTER FILE
	draw_shadow_text(hud_x, current_y, "--- REGISTER FILE ---", pal_pink);
	int reg_start_y = current_y + 15;
	for (int i = 0; i < 32; i++) {
		int cx = (i < 16) ? hud_x : hud_x + 105;
		int cy = reg_start_y + ((i % 16) * 11);

		sprintf(buf, "X%02d:", i);
		draw_shadow_text(cx, cy, buf, pal_red);

		sprintf(buf, "%08X", regs.read_x(i));
		draw_shadow_text(cx + 35, cy, buf, pal_white);
	}

	current_y = reg_start_y + (16 * 11) + 12;

	// TRACE LOG
	draw_shadow_text(hud_x, current_y, "--- TRACE LOG ---", pal_pink);
	current_y += 15;

	// Most recently recorded history entry == the instruction that just executed.
	int active_idx = (regs.history_pos() + Registers::HISTORY_SIZE - 1) % Registers::HISTORY_SIZE;
	const HistoryEntry &active = regs.history_at(active_idx);
	sprintf(buf, "ACTIVE: %08X %s", active.instr, active.mnemonic);
	draw_shadow_text(hud_x, current_y, buf, pal_pink);
	current_y += 12;

	sprintf(buf, "CURR PC: %08X", regs.get_pc());
	draw_shadow_text(hud_x, current_y, buf, pal_white);

	for (int i = 0; i < 4; i++) {
		int pos = (regs.history_pos() + i) % Registers::HISTORY_SIZE;
		const HistoryEntry &h = regs.history_at(pos);
		sprintf(buf, "%08X: %s", h.pc, h.mnemonic);
		draw_shadow_text(hud_x, (current_y + 15) + (i * 11), buf, pal_stats);
	}

	SDL_UpdateTexture(texture, nullptr, screen, TOTAL_W * 4);
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

std::vector<RawKeyEvent> Gui::poll_input()
{
	std::vector<RawKeyEvent> events;

	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) exit(0);
		if (e.type == SDL_KEYDOWN) events.push_back({(uint32_t)e.key.keysym.sym, true});
		if (e.type == SDL_KEYUP) events.push_back({(uint32_t)e.key.keysym.sym, false});
	}

	return events;
}

void Gui::draw_char(uint32_t *p, int x, int y, char c, uint32_t col)
{
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

void Gui::draw_string(uint32_t *p, int x, int y, const char *s, uint32_t c)
{
	while (*s) { draw_char(p, x, y, *s++, c); x += 8; }
}

void Gui::init_font()
{
	std::memset(font8x8, 0, sizeof(font8x8));
	auto set_char = [this](char c, std::initializer_list<uint8_t> rows) {
		int i = 0;
		for (uint8_t row : rows) {
			if (i < 8) font8x8[(uint8_t)c][i++] = row;
		}
	};

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
	set_char('J', {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00});
	set_char('K', {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00});
	set_char('Q', {0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x0E, 0x00});
	set_char('W', {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00});
	set_char('X', {0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00});
	set_char('Z', {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00});
	set_char('?', {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00});

	set_char('n', {0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00});
	set_char('s', {0x00, 0x00, 0x3C, 0x60, 0x3C, 0x06, 0x3C, 0x00});
	set_char('i', {0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00});
	set_char('.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00});
	set_char('/', {0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00});
	set_char(':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00});
	set_char('-', {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00});
	set_char(' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}
