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
	// the window's real pixel size, a set 1920x1080 (not just a default
	// -- see init() below, window is not resizable). 640x360 is exactly
	// 1920x1080's own 16:9 aspect ratio, giving one clean scale_x==
	// scale_y==3.0 (see render()) instead of two DIFFERENT scale
	// factors -- a mismatched design-space aspect ratio was stretching
	// everything non-uniformly (most visibly the Doom game view itself,
	// rendered at the wrong aspect), not clipping it, even though it
	// looked like content wasn't "fitting" right. (Was 960x540/scale
	// 2.0 -- every layout constant in render()'s HUD section was
	// rescaled by 2/3 alongside this shrink, so the on-screen result
	// stays the same size, just recomputed against a smaller design
	// space.) Content past y=DESIGN_H is clipped off the bottom of the
	// canvas regardless of window size, so this is a hard bound, not
	// just a hint.
	static constexpr int DESIGN_W = 640;
	static constexpr int DESIGN_H = 360;

	Gui();
	~Gui();

	bool init(int window_w = 1920, int window_h = 1080);

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
	// Separate x/y scale (not one uniform "font_scale") so a glyph can be
	// stretched taller without also getting wider -- scale_y < 0 means
	// "match scale_x", the common case (every call site except the
	// register file's taller-but-not-wider text just passes one value).
	void draw_char(int x, int y, char c, uint32_t color, float scale_x_ = 1.0f, float scale_y_ = -1.0f);
	void draw_string(int x, int y, const char *str, uint32_t color, float scale_x_ = 1.0f, float scale_y_ = -1.0f);
};
