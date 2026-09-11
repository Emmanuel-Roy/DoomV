#pragma once
#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>
#include "snapshot.hpp"

// One thing that happened in the window, before any interpretation.
//
// This used to be a keysym and a pressed flag, which is all DOOM needs and
// not enough for anything else. Three different consumers now want three
// different views of the same keypress:
//
//   * an evdev keyboard wants the *scancode* -- the physical key -- because
//     that is what a Linux keyboard reports and the guest applies its own
//     keymap to it. A keysym has already had a layout applied and cannot be
//     un-applied.
//   * a serial console wants the *character*, with the host's layout,
//     modifiers and dead keys resolved. That is SDL_TEXTINPUT's job and it
//     is not reconstructible from a keysym plus a shift bit -- which is
//     what the old hand-rolled table tried to do, and why it handled a US
//     layout and nothing else.
//   * DOOM wants the keysym, as before.
//
// So the event carries all three and each consumer takes what it needs.
struct RawInputEvent {
	enum class Kind : uint8_t {
		Key,          // a physical key went down or up
		Text,         // composed text arrived (layout and modifiers applied)
		MouseMotion,  // relative movement
		MouseButton,
		MouseWheel,
	};

	Kind kind = Kind::Key;

	uint32_t sdl_keysym = 0;    // Key: SDL_Keycode
	uint32_t sdl_scancode = 0;  // Key: SDL_Scancode, i.e. which physical key
	uint16_t mods = 0;          // Key: SDL_Keymod at the time of the event
	bool pressed = false;       // Key, MouseButton
	// Key: this is the host's auto-repeat rather than a new press. A field
	// of its own and not a spare bit in `mods` -- every bit up to 0x8000
	// is a real SDL_Keymod, 0x8000 being KMOD_SCROLL, so borrowing one
	// would mean Scroll Lock silently marking every keystroke a repeat.
	bool repeat = false;

	// Text: UTF-8, NUL-terminated. SDL's own buffer is 32 bytes; a single
	// composed character is at most 4, and this never holds more than one.
	char text[8] = {0};

	int dx = 0, dy = 0;         // MouseMotion, MouseWheel
	int button = 0;             // MouseButton: SDL_BUTTON_*
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
	std::vector<RawInputEvent> poll_input();

	// Relative mouse mode: the pointer is hidden and confined, and motion
	// arrives as deltas with no edges to run into. That is what a guest
	// with its own cursor needs, and it is also a trap for the person at
	// the keyboard -- so it is off until asked for, and the caller is
	// expected to give them a way back out. See the Ctrl+Alt+G handling in
	// DoomSystem::run.
	void set_mouse_captured(bool on);
	bool mouse_captured() const { return captured; }

	// Give a Linux framebuffer the whole window instead of the dashboard's
	// game box. Off by default: the dashboard is the point of this window,
	// and a console is legible enough in the box to work in. It exists
	// because *reading* a 1024x768 console in a 840x525 box means reading
	// 8x16 text at 0.68x, and some of the time that is what you need to do.
	void toggle_fb_fullscreen() { fb_full = !fb_full; }

	// Write the composed canvas -- dashboard, panels, guest display and
	// all -- to a PPM. This is -fbdump's trick applied to the window
	// instead of the guest's framebuffer, and for the same reason: a
	// screenshot of an SDL window is not evidence. CopyFromScreen copies
	// whatever is on top of that rectangle on the actual screen, and
	// PrintWindow returns black for GPU-composited content. This comes
	// from the pixel buffer the renderer is handed, so it is exactly what
	// was drawn and cannot be the wrong window.
	void set_canvas_dump(const char *path) { canvas_dump_path = path; }

private:
	SDL_Window *window = nullptr;
	SDL_Renderer *renderer = nullptr;
	SDL_Texture *texture = nullptr;

	bool captured = false;
	bool fb_full = false;
	std::string canvas_dump_path;
	int canvas_dump_frames = 0;
	void dump_canvas();

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
