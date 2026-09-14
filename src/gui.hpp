#pragma once
#include <SDL2/SDL.h>
#include <atomic>
#include <cstdint>
#include <mutex>
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
	// MouseMotion, MouseButton, MouseWheel: where the pointer is in the
	// guest's own framebuffer pixels. poll_input only reports mouse events
	// that happen over the display -- the rest of the window is the
	// dashboard's, and a click on a register is not a click in the guest --
	// so these are always inside [0, w) x [0, h).
	int x = 0, y = 0;
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

	// The guest display's size in pixels: DOOM's 320x200 or the Linux
	// framebuffer's. Fixed for a run, and set before any thread starts.
	void set_guest_display(int w, int h);
	// An absolute pointer (Linux, through the virtio tablet) rather than a
	// relative one (DOOM's mouse look). Decides what capturing the mouse
	// means -- see set_mouse_captured.
	void set_absolute_pointer(bool on) { absolute_pointer = on; }

	// The window is drawn by three threads, each owning one job:
	//
	//   * the display thread hands over finished guest frames (submit_frame)
	//   * the dashboard thread draws the CSRs, register file and trace log
	//     into a layer of their own (render_dashboard)
	//   * the window thread -- the one that owns SDL -- composites whichever
	//     of those changed and presents (present)
	//
	// They meet at two swaps under two small locks, and neither producer
	// waits for the window: a frame or a dashboard nobody has composited yet
	// is simply replaced by the next one.

	// Display thread. `pixels` must be set_guest_display's size; it is
	// swapped with the pending frame, so it comes back holding a buffer of
	// the same size to reuse.
	void submit_frame(std::vector<uint32_t> &pixels);
	// Dashboard thread.
	void render_dashboard(const Snapshot &snap);
	// Window thread.
	void present();
	std::vector<RawInputEvent> poll_input();

	// Capture confines the host pointer to the display area -- not the
	// whole window -- and hides it, since the guest draws its own. For
	// DOOM it also switches to relative motion, so the view can keep
	// turning with no edge to run into. That is a trap for the person at
	// the keyboard, so it is off until asked for, and the caller is
	// expected to give them a way back out. See the Ctrl+Alt+G handling in
	// DoomSystem::run.
	void set_mouse_captured(bool on);
	bool mouse_captured() const { return captured; }

	// Give a Linux framebuffer the whole window instead of sharing it with
	// the dashboard. Off by default: the dashboard is the point of this
	// window, and at 1920x1080 the compact layout already shows the console
	// at 1:1 beside it. It matters on a smaller window, where the dashboard
	// falls back to letterboxing the console into DOOM's display box.
	void toggle_fb_fullscreen();

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
	bool absolute_pointer = false;
	bool fb_full = false;          // window thread only
	uint32_t buttons_down = 0;     // presses that were forwarded, by SDL button

	int guest_w = Memory::FB_W, guest_h = Memory::FB_H;
	bool guest_is_linux() const { return guest_w != Memory::FB_W || guest_h != Memory::FB_H; }

	struct Rect { int x, y, w, h; };
	// Where the guest display goes on the canvas, for the current layout.
	// One definition shared by the compositor, the dashboard and the
	// pointer mapping, so a click lands on the pixel that is drawn there.
	Rect display_rect(bool full, bool *compact = nullptr) const;
	// Window coordinates to guest framebuffer pixels. False if the point
	// is outside the display.
	bool map_pointer(int wx, int wy, int &gx, int &gy) const;
	void apply_mouse_rect();

	// Display layer. `frame` is the latest submitted frame, `frame_shown`
	// the one on the canvas; present() swaps them when frame_seq moves.
	std::mutex frame_mutex;
	std::vector<uint32_t> frame, frame_shown;
	uint64_t frame_seq = 0, frame_seen = ~0ull;
	void blit_display(const Rect &r);

	// Dashboard layer, canvas-sized. The dashboard thread draws into
	// dash_work and swaps it into dash_ready; present() swaps that into
	// dash_shown.
	std::mutex dash_mutex;
	std::vector<uint32_t> dash_work, dash_ready, dash_shown;
	uint64_t dash_seq = 0, dash_seen = ~0ull;
	bool composed_full = false;
	std::string canvas_dump_path;
	int canvas_dump_frames = 0;
	void dump_canvas();

	int canvas_w = 0, canvas_h = 0;
	float scale_x = 1.0f, scale_y = 1.0f;
	// Screen pixels per layout unit for text, set by render() for the
	// layout in use. It is scale_x/scale_y for the design-unit dashboard,
	// and 1 for the compact Linux one, which is laid out in real pixels so
	// that its text can be drawn at exactly one pixel per font pixel.
	float text_ux = 1.0f, text_uy = 1.0f;
	std::vector<uint32_t> screen_buf;   // the composited canvas, window thread only
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
	// `track` is extra letter spacing, in layout units, on top of the
	// natural 8*scale_x_ pitch -- the compact layout needs a pitch that is
	// not a multiple of the glyph width.
	void draw_string(int x, int y, const char *str, uint32_t color, float scale_x_ = 1.0f, float scale_y_ = -1.0f, int track = 0);
};
