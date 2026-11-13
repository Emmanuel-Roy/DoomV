// Performance counters. See docs/PERFORMANCE.md.
#pragma once
#include <stdint.h>

enum perf_phase {
	PERF_TIC,       // G_Ticker: game logic (35 per second when keeping up)
	PERF_BSP,       // R_RenderBSPNode: walk the map, draw walls
	PERF_PLANES,    // R_DrawPlanes: floors and ceilings
	PERF_MASKED,    // R_DrawMasked: sprites, masked walls, the weapon
	PERF_HUD,       // ST_Drawer: the status bar
	PERF_PRESENT,   // backend_present: palette conversion + SPI to the LCD
	PERF_LINK,      // lock-step hashes
	PERF_NPHASES
};

typedef struct {
	uint32_t frames;            // frames in this window
	uint32_t tics;              // game tics in this window
	uint32_t window_us;         // length of the window
	uint32_t frame_us_max;      // slowest frame
	uint32_t phase_us[PERF_NPHASES];  // total time per phase in the window
	int zone_used, zone_size;   // bytes, live data only (cache counts as free)
	int zone_peak;
} perf_window_t;

// Peak use of the renderer's fixed-size arrays since boot. These arrays are
// a big slice of RAM, so their sizes are set from these measurements (see
// docs/MEMORY.md); the sim's -explore mode is how the peaks are found.
typedef struct { int visplanes, openings, drawsegs, vissprites; } perf_limits_t;
const perf_limits_t *perf_limits(void);

// Retired instructions since the last call, in game logic (G_Ticker) and in
// rendering (BSP, planes, sprites, status bar); then resets both.
void perf_take_instret(uint32_t *tic, uint32_t *render);

void perf_frame_rendered(void);
void perf_add_present(uint64_t cycles);
void perf_add_link(uint64_t cycles);

// The last complete one-second window (updated in perf_frame_rendered).
const perf_window_t *perf_last_window(void);
// Totals since boot, for the end-of-run benchmark summary.
const perf_window_t *perf_totals(void);
// Format a window as one line: "fps 21.4 | frame 46.7 ms (max 61) | ..."
void perf_format(const perf_window_t *w, char *out, int outlen);
