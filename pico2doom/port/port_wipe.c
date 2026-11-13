// Replaces doomgeneric's f_wipe.c: the same screen melt, in a third of the
// memory.
//
// The original melt keeps three 64,000-byte screens: the old picture
// (wipe_scr_start), the new one (wipe_scr_end), and a column-major scratch
// copy it makes of each while setting up. Peak: 192 KB on top of the screen
// itself -- more than a Pico 2 has free once a level is loaded.
//
// This version keeps one extra screen, the new picture, and melts the old
// one *in place* in I_VideoBuffer:
//
//   * wipe_EndScreen swaps the two pictures instead of copying, so the
//     screen holds the old picture and the side buffer holds the new one.
//   * Each step, a column's old picture slides down by dy. The rows it
//     slides into are always below the rows it comes from, so copying
//     bottom-to-top within the column never reads a row it already wrote.
//     The dy rows uncovered at the top are filled from the new picture.
//
// The column offsets come from the same M_Random() calls in the same order,
// so the melt looks identical and the menu RNG stays in step with vanilla
// (and with DoomV, which runs this file too).
//
// Even 64 KB is a lot on a Pico 2 with a level loaded, so the buffer is only
// borrowed if the zone can spare it (Z_TryMalloc, patches/08). If not, the
// new picture just appears. The decision depends only on the zone's size and
// history, so a DoomV replay with the same zone size makes the same choice.

#include <stdio.h>
#include <string.h>

#include "port.h"

#include "doomtype.h"
#include "i_video.h"
#include "m_random.h"
#include "v_video.h"
#include "z_zone.h"
#include "f_wipe.h"

#define W (SCREENWIDTH / 2)   // the melt moves 2-pixel columns (shorts)
#define H SCREENHEIGHT

static byte *wipe_new;        // the new picture, SCREENWIDTH*SCREENHEIGHT
static int col_y[W];          // rows of each column already melted
static boolean going;

int wipe_StartScreen(int x, int y, int width, int height)
{
	(void)x; (void)y; (void)width; (void)height;
#ifdef Z_HAVE_TRYMALLOC
	wipe_new = Z_TryMalloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
#else
	wipe_new = Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);   // stock engine (A/B builds)
#endif
	if (wipe_new)
		I_ReadScreen(wipe_new);    // holds the *old* picture until EndScreen
	{
		extern int Z_FreeMemory(void);
		char line[64];
		snprintf(line, sizeof line, "wipe: %s (zone free %d)", wipe_new ? "melt" : "no room, cut", Z_FreeMemory());
		backend_log(line);
	}
	return 0;
}

int wipe_EndScreen(int x, int y, int width, int height)
{
	(void)x; (void)y; (void)width; (void)height;
	if (!wipe_new) return 0;   // no melt: the screen already shows the new picture
	// Screen = new picture, side buffer = old. Swap them word by word.
	uint32_t *a = (uint32_t *)I_VideoBuffer, *b = (uint32_t *)wipe_new;
	for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT / 4; i++) {
		uint32_t t = a[i]; a[i] = b[i]; b[i] = t;
	}
	return 0;
}

static void melt_init(void)
{
	col_y[0] = -(M_Random() % 16);
	for (int i = 1; i < W; i++) {
		int r = (M_Random() % 3) - 1;
		col_y[i] = col_y[i - 1] + r;
		if (col_y[i] > 0) col_y[i] = 0;
		else if (col_y[i] == -16) col_y[i] = -15;
	}
}

static boolean melt_step(int ticks)
{
	uint16_t *scr = (uint16_t *)I_VideoBuffer;
	const uint16_t *fresh = (const uint16_t *)wipe_new;
	boolean done = true;

	while (ticks--) {
		for (int i = 0; i < W; i++) {
			int y = col_y[i];
			if (y < 0) {
				col_y[i]++;
				done = false;
			} else if (y < H) {
				int dy = (y < 16) ? y + 1 : 8;
				if (y + dy >= H) dy = H - y;
				// Slide the old picture down by dy, bottom row first.
				for (int r = H - 1; r >= y + dy; r--)
					scr[r * W + i] = scr[(r - dy) * W + i];
				// Uncover dy rows of the new picture at the top.
				for (int r = y; r < y + dy; r++)
					scr[r * W + i] = fresh[r * W + i];
				col_y[i] = y + dy;
				done = false;
			}
		}
	}
	return done;
}

int wipe_ScreenWipe(int wipeno, int x, int y, int width, int height, int ticks)
{
	(void)wipeno; (void)x; (void)y;   // only the melt is ever used by DOOM
	if (!wipe_new) return 1;          // no room for a melt: done at once
	if (!going) {
		going = true;
		melt_init();
	}
	V_MarkRect(0, 0, width, height);
	if (melt_step(ticks)) {
		going = false;
		Z_Free(wipe_new);
		wipe_new = NULL;
	}
	return !going;
}
