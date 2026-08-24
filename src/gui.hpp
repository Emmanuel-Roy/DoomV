#pragma once
#include <SDL2/SDL.h>
#include <cstdint>
#include <vector>
#include "snapshot.hpp"

struct RawKeyEvent {
	uint32_t sdl_keysym;
	bool pressed;
};

class Gui {
public:
	// Layout is designed in these units; actual canvas is scaled up to
	// whatever the window's real pixel size is (default 1280x720, an
	// exact 2x multiple, so the baseline is pixel-crisp -- see render()).
	static constexpr int DESIGN_W = 640;
	static constexpr int DESIGN_H = 360;

	Gui();
	~Gui();

	bool init(int window_w = 1280, int window_h = 720);

	void render(const Snapshot &snap);
	std::vector<RawKeyEvent> poll_input();

private:
	SDL_Window *window = nullptr;
	SDL_Renderer *renderer = nullptr;
	SDL_Texture *texture = nullptr;

	int canvas_w = 0, canvas_h = 0;
	float scale_x = 1.0f, scale_y = 1.0f;
	std::vector<uint32_t> screen_buf;
	void resize_canvas_if_needed();

	uint8_t font8x8[128][8];
	void init_font();
	// font_scale shrinks a glyph's rendered size without changing where its
	// origin sits in the design-unit coordinate system -- lets one region
	// (the register grid) pack text tighter than the rest of the HUD.
	void draw_char(int x, int y, char c, uint32_t color, float font_scale = 1.0f);
	void draw_string(int x, int y, const char *str, uint32_t color, float font_scale = 1.0f);

	uint32_t last_sync;
	float dashboard_fps = 0.0f;
};
