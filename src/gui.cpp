#include "gui.hpp"
#include "registers.hpp"
#include "memory.hpp"
#include "debugger.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
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

bool Gui::init(int window_w, int window_h)
{
	if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;

	window = SDL_CreateWindow("RISC-V Doom SoC",
	                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                          window_w, window_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	if (!window) return false;

	// No VSYNC: it locked every loop iteration to ~60Hz even though the
	// instruction burst itself finishes in a small fraction of that
	// budget, wasting most of each frame's time doing nothing. Uncapped,
	// the loop runs as fast as the burst+render actually take.
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // nearest-neighbor -- sharp blocks, never blurry

	resize_canvas_if_needed();
	last_sync = SDL_GetTicks();

	return (renderer && texture);
}

void Gui::resize_canvas_if_needed()
{
	int w, h;
	SDL_GetWindowSize(window, &w, &h);
	if (w == canvas_w && h == canvas_h) return;

	canvas_w = w;
	canvas_h = h;
	scale_x = (float)canvas_w / (float)DESIGN_W;
	scale_y = (float)canvas_h / (float)DESIGN_H;
	screen_buf.resize((size_t)canvas_w * (size_t)canvas_h);

	if (texture) SDL_DestroyTexture(texture);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
	                            SDL_TEXTUREACCESS_STREAMING, canvas_w, canvas_h);
}

void Gui::render(const Registers &regs, Memory &mem, const Debugger &dbg)
{
	resize_canvas_if_needed();

	uint32_t now = SDL_GetTicks();
	uint32_t delta = now - last_sync;
	if (delta == 0) delta = 1;
	dashboard_fps = 1000.0f / (float)delta;
	last_sync = now;

	std::fill(screen_buf.begin(), screen_buf.end(), 0x876A96);

	uint32_t pal_pink  = 0xD580B8;
	uint32_t pal_white = 0xD7D0D0;
	uint32_t pal_red   = 0xB95167;
	uint32_t pal_dark  = 0x25080C;
	uint32_t pal_stats = 0xC3A9C4;

	// GAME SCREEN: design box is 400x250 at (15,55), scaled to canvas
	// pixels as one box, then sampled directly from the native 320x200
	// framebuffer into that box in a single scale step (avoids chaining
	// two scaling passes, which would leave gaps or blur at odd sizes).
	int box_x = (int)(15 * scale_x);
	int box_y = (int)(55 * scale_y);
	int box_w = (int)(400 * scale_x);
	int box_h = (int)(250 * scale_y);

	// Bilinear, not nearest-neighbor: at native 320x200 scaled ~3-4x, hard
	// pixel blocks looked wrong for the game view (dashboard text stays
	// sharp block-fills on purpose, this is just the rendered scene).
	// Fixed-point (8-bit fraction) so the per-pixel blend is pure integer
	// math, no floats in the hot loop.
	struct Sample { int i0, i1; uint32_t frac; };
	static std::vector<Sample> sx_lut, sy_lut;
	static int last_box_w = -1, last_box_h = -1;
	if (box_w != last_box_w || box_h != last_box_h) {
		sx_lut.resize(box_w > 0 ? box_w : 0);
		sy_lut.resize(box_h > 0 ? box_h : 0);
		for (int x = 0; x < box_w; x++) {
			float src = ((float)x + 0.5f) * Memory::FB_W / box_w - 0.5f;
			int i0 = (int)std::floor(src);
			float frac = src - (float)i0;
			if (i0 < 0) { i0 = 0; frac = 0.0f; }
			int i1 = (i0 + 1 < Memory::FB_W) ? i0 + 1 : i0;
			sx_lut[x] = { i0, i1, (uint32_t)(frac * 256.0f) };
		}
		for (int y = 0; y < box_h; y++) {
			float src = ((float)y + 0.5f) * Memory::FB_H / box_h - 0.5f;
			int i0 = (int)std::floor(src);
			float frac = src - (float)i0;
			if (i0 < 0) { i0 = 0; frac = 0.0f; }
			int i1 = (i0 + 1 < Memory::FB_H) ? i0 + 1 : i0;
			sy_lut[y] = { i0, i1, (uint32_t)(frac * 256.0f) };
		}
		last_box_w = box_w;
		last_box_h = box_h;
	}

	const uint32_t *fb32 = reinterpret_cast<const uint32_t *>(mem.framebuffer());
	for (int y = 0; y < box_h; y++) {
		int ty = box_y + y;
		if (ty < 0 || ty >= canvas_h) continue;

		const Sample &ys = sy_lut[y];
		const uint32_t *row0 = fb32 + ys.i0 * Memory::FB_W;
		const uint32_t *row1 = fb32 + ys.i1 * Memory::FB_W;
		uint32_t *dst_row = &screen_buf[(size_t)ty * canvas_w];

		for (int x = 0; x < box_w; x++) {
			int tx = box_x + x;
			if (tx < 0 || tx >= canvas_w) continue;

			const Sample &xs = sx_lut[x];
			uint32_t p00 = row0[xs.i0], p10 = row0[xs.i1];
			uint32_t p01 = row1[xs.i0], p11 = row1[xs.i1];

			uint32_t out = 0;
			for (int shift = 16; shift >= 0; shift -= 8) {
				uint32_t c00 = (p00 >> shift) & 0xFF, c10 = (p10 >> shift) & 0xFF;
				uint32_t c01 = (p01 >> shift) & 0xFF, c11 = (p11 >> shift) & 0xFF;
				uint32_t top = c00 * (256 - xs.frac) + c10 * xs.frac;
				uint32_t bot = c01 * (256 - xs.frac) + c11 * xs.frac;
				uint32_t chan = (top * (256 - ys.frac) + bot * ys.frac) >> 16;
				out |= chan << shift;
			}
			dst_row[tx] = out;
		}
	}

	char buf[64];
	int hud_x = 425;

	auto draw_shadow_text = [&](int x, int y, const char *s, uint32_t col) {
		draw_string(x + 1, y + 1, s, pal_dark);
		draw_string(x, y, s, col);
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

	SDL_UpdateTexture(texture, nullptr, screen_buf.data(), canvas_w * 4);
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

void Gui::draw_char(int x, int y, char c, uint32_t col)
{
	if ((uint8_t)c >= 128) return;

	int bw = (int)(scale_x + 0.5f); if (bw < 1) bw = 1;
	int bh = (int)(scale_y + 0.5f); if (bh < 1) bh = 1;

	for (int r = 0; r < 8; r++) {
		uint8_t b = font8x8[(uint8_t)c][r];
		for (int cl = 0; cl < 8; cl++) {
			if (!(b & (0x80 >> cl))) continue;

			int px = (int)((x + cl) * scale_x);
			int py = (int)((y + r) * scale_y);
			for (int by = 0; by < bh; by++) {
				int ty = py + by;
				if (ty < 0 || ty >= canvas_h) continue;
				uint32_t *row = &screen_buf[(size_t)ty * canvas_w];
				for (int bx = 0; bx < bw; bx++) {
					int tx = px + bx;
					if (tx < 0 || tx >= canvas_w) continue;
					row[tx] = col;
				}
			}
		}
	}
}

void Gui::draw_string(int x, int y, const char *s, uint32_t c)
{
	while (*s) { draw_char(x, y, *s++, c); x += 8; }
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
