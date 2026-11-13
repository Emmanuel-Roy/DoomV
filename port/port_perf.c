// Where a frame's time goes, measured without touching the engine.
//
// Each phase is an engine function wrapped at build time (see PORT_WRAPS in
// port.cmake): __wrap_X reads the cycle counter, calls __real_X, reads it
// again. The counter is mcycle on the Pico, so the overhead is two CSR
// reads per call -- a few dozen cycles a frame.
//
// The same windows also count *retired* instructions (minstret, see
// port_instret.h) for lock-step: game logic and rendering separately, per
// frame, minus whatever ran inside the platform's DG_* calls (port_link.c),
// so that only code the Pico and DoomV share is counted.
//
// Once a second the window is closed, logged to the console (USB serial on
// the Pico), and drawn in the 40 spare rows under the 320x200 picture.

#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "d_player.h"
#include "r_defs.h"
#include "r_bsp.h"
#include "r_things.h"

#include "port.h"
#include "port_perf.h"
#include "port_instret.h"
#include "port_link.h"

int port_zone_used(void);
int port_zone_size(void);

static uint64_t phase_cycles[PERF_NPHASES];
static uint32_t win_frames, win_tics, win_frame_max_us;
static uint64_t win_start, last_frame;
static int zone_peak;
static perf_window_t last, total;

static inline uint32_t to_us(uint64_t cycles)
{
	return (uint32_t)(cycles / backend_cycles_per_us());
}

// Retired instructions this frame: game logic, and rendering.
static uint32_t instret_tic, instret_render;

// Time a phase and count its retired instructions. The minstret reads sit
// directly around the call, so the window holds only the engine function
// (plus anything it calls) -- code the Pico and DoomV share.
#define TIMED(phase, region, call) do { \
	uint64_t t0_ = backend_cycles(); \
	uint32_t x0_ = port_link_excluded_instret(); \
	uint32_t i0_ = port_instret(); \
	call; \
	uint32_t i1_ = port_instret(); \
	region += (i1_ - i0_) - (port_link_excluded_instret() - x0_); \
	phase_cycles[phase] += backend_cycles() - t0_; \
} while (0)

void perf_take_instret(uint32_t *tic, uint32_t *render)
{
	*tic = instret_tic;
	*render = instret_render;
	instret_tic = instret_render = 0;
}

extern void __real_G_Ticker(void);
#ifdef PORT_FUNCPROF
// The lock-step drill-down (port_funcprof.c): profile each game-logic window.
void port_funcprof_begin(void);
void port_funcprof_end(uint32_t frame, uint32_t window_instret);
void __wrap_G_Ticker(void)
{
	win_tics++;
	uint32_t before = instret_tic;
	port_funcprof_begin();
	TIMED(PERF_TIC, instret_tic, __real_G_Ticker());
	port_funcprof_end(port_link_frames() + 1, instret_tic - before);
}
#else
void __wrap_G_Ticker(void) { win_tics++; TIMED(PERF_TIC, instret_tic, __real_G_Ticker()); }
#endif

extern void __real_R_RenderBSPNode(int bspnum);
void __wrap_R_RenderBSPNode(int bspnum) { TIMED(PERF_BSP, instret_render, __real_R_RenderBSPNode(bspnum)); }

extern void __real_R_DrawPlanes(void);
void __wrap_R_DrawPlanes(void) { TIMED(PERF_PLANES, instret_render, __real_R_DrawPlanes()); }

// r_plane.c keeps these private; their sizes may be changed by patches.
extern visplane_t visplanes[], *lastvisplane;
extern short openings[], *lastopening;
static perf_limits_t limits;

static void note_limits(void)
{
	int v = (int)(lastvisplane - visplanes), o = (int)(lastopening - openings);
	int d = (int)(ds_p - drawsegs), s = (int)(vissprite_p - vissprites);
	if (v > limits.visplanes) limits.visplanes = v;
	if (o > limits.openings) limits.openings = o;
	if (d > limits.drawsegs) limits.drawsegs = d;
	if (s > limits.vissprites) limits.vissprites = s;
}

const perf_limits_t *perf_limits(void) { return &limits; }

extern void __real_R_DrawMasked(void);
void __wrap_R_DrawMasked(void)
{
	TIMED(PERF_MASKED, instret_render, __real_R_DrawMasked());
	note_limits();   // the frame's planes, openings, segs and sprites are all in place now
}

extern void __real_ST_Drawer(boolean fullscreen, boolean refresh);
void __wrap_ST_Drawer(boolean fullscreen, boolean refresh) { TIMED(PERF_HUD, instret_render, __real_ST_Drawer(fullscreen, refresh)); }

void perf_add_present(uint64_t cycles) { phase_cycles[PERF_PRESENT] += cycles; }
void perf_add_link(uint64_t cycles) { phase_cycles[PERF_LINK] += cycles; }

static void close_window(uint64_t now)
{
	last.frames = win_frames;
	last.tics = win_tics;
	last.window_us = to_us(now - win_start);
	last.frame_us_max = win_frame_max_us;
	for (int i = 0; i < PERF_NPHASES; i++) {
		last.phase_us[i] = to_us(phase_cycles[i]);
		total.phase_us[i] += last.phase_us[i];
		phase_cycles[i] = 0;
	}
	last.zone_used = port_zone_used();
	last.zone_size = port_zone_size();
	last.zone_peak = zone_peak;

	total.frames += last.frames;
	total.tics += last.tics;
	total.window_us += last.window_us;
	if (last.frame_us_max > total.frame_us_max) total.frame_us_max = last.frame_us_max;
	total.zone_used = last.zone_used;
	total.zone_size = last.zone_size;
	total.zone_peak = zone_peak;

	win_frames = win_tics = win_frame_max_us = 0;
	win_start = now;

	char line[160];
	perf_format(&last, line, sizeof line);
	backend_log(line);
	backend_hud(line);
}

void perf_frame_rendered(void)
{
	uint64_t now = backend_cycles();
	if (win_start == 0) win_start = last_frame = now;

	uint32_t frame_us = to_us(now - last_frame);
	last_frame = now;
	if (frame_us > win_frame_max_us) win_frame_max_us = frame_us;
	win_frames++;

	int used = port_zone_used();
	if (used > zone_peak) zone_peak = used;
	total.zone_used = used;
	total.zone_size = port_zone_size();
	total.zone_peak = zone_peak;

	if (to_us(now - win_start) >= 1000000) close_window(now);
}

void perf_format(const perf_window_t *w, char *out, int outlen)
{
	if (!w->frames || !w->window_us) { snprintf(out, outlen, "perf: no frames yet"); return; }
	unsigned f = w->frames;
	// Per-frame averages in tenths of a millisecond (no floating point: the
	// Hazard3 cores have no FPU, and printf("%f") would pull in soft-float).
	#define MS10(us) ((unsigned)((us) / f / 100))
	unsigned fps10 = (unsigned)((uint64_t)f * 10000000u / w->window_us);
	unsigned frame10 = (unsigned)(w->window_us / f / 100);
	snprintf(out, outlen,
		"fps %u.%u | frame %u.%u ms (max %u) | tic %u.%u bsp %u.%u planes %u.%u "
		"sprites %u.%u hud %u.%u lcd %u.%u | zone %d/%d KB (peak %d)",
		fps10 / 10, fps10 % 10, frame10 / 10, frame10 % 10, (unsigned)(w->frame_us_max / 1000),
		MS10(w->phase_us[PERF_TIC]) / 10, MS10(w->phase_us[PERF_TIC]) % 10,
		MS10(w->phase_us[PERF_BSP]) / 10, MS10(w->phase_us[PERF_BSP]) % 10,
		MS10(w->phase_us[PERF_PLANES]) / 10, MS10(w->phase_us[PERF_PLANES]) % 10,
		MS10(w->phase_us[PERF_MASKED]) / 10, MS10(w->phase_us[PERF_MASKED]) % 10,
		MS10(w->phase_us[PERF_HUD]) / 10, MS10(w->phase_us[PERF_HUD]) % 10,
		MS10(w->phase_us[PERF_PRESENT]) / 10, MS10(w->phase_us[PERF_PRESENT]) % 10,
		w->zone_used / 1024, w->zone_size / 1024, w->zone_peak / 1024);
	#undef MS10
}

const perf_window_t *perf_last_window(void) { return &last; }
const perf_window_t *perf_totals(void) { return &total; }
