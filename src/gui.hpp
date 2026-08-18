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
	static constexpr int TOTAL_W = 640;
	static constexpr int TOTAL_H = 360;

	Gui();
	~Gui();

	bool init(int scale_factor);

	void render(const Registers &regs, Memory &mem, const Debugger &dbg);
	std::vector<RawKeyEvent> poll_input();

private:
	SDL_Window *window = nullptr;
	SDL_Renderer *renderer = nullptr;
	SDL_Texture *texture = nullptr;

	uint8_t font8x8[128][8];
	void init_font();
	void draw_char(uint32_t *pixels, int x, int y, char c, uint32_t color);
	void draw_string(uint32_t *pixels, int x, int y, const char *str, uint32_t color);

	uint32_t last_sync;
	float dashboard_fps = 0.0f;
};
