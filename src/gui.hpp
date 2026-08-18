#pragma once
#include <SDL2/SDL.h>
#include <cstdint>
#include <vector>

class Registers;
class Memory;
class Debugger;

struct RawKeyEvent {
	uint32_t sdl_keysym;
	bool pressed;
};

class Gui {
public:
	// Layout is designed in these units; actual canvas is scaled up to
	// whatever the window's real pixel size is (default 1920x1080, an
	// exact 3x multiple, so the baseline is pixel-crisp -- see render()).
	static constexpr int DESIGN_W = 640;
	static constexpr int DESIGN_H = 360;

	Gui();
	~Gui();

	bool init(int window_w = 1920, int window_h = 1080);

	void render(const Registers &regs, Memory &mem, const Debugger &dbg);
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
	void draw_char(int x, int y, char c, uint32_t color);
	void draw_string(int x, int y, const char *str, uint32_t color);

	uint32_t last_sync;
	float dashboard_fps = 0.0f;
};
