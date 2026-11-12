// Replaces doomgeneric's i_video.c.
//
// Stock doomgeneric renders into DOOM's 8-bit screen, then expands every
// frame into DG_ScreenBuffer, a 32-bit-per-pixel copy (320x200x4 = 256 KB)
// that the platform reads. On a chip with 520 KB of SRAM that copy alone is
// half the machine, so it is gone: the backend receives the 8-bit frame and
// the palette, and converts as it sends (the Pico converts one strip of
// lines at a time straight into its SPI DMA buffers).
//
// I_VideoBuffer is a static array rather than a Z_Malloc, so the zone does
// not have to find 64 KB of contiguous space for it.

#include <string.h>

#include "doomtype.h"
#include "i_video.h"
#include "v_video.h"
#include "z_zone.h"
#include "tables.h"

#include "port.h"
#include "port_link.h"
#include "port_perf.h"

static byte screen_8bit[SCREENWIDTH * SCREENHEIGHT];
byte *I_VideoBuffer = screen_8bit;

boolean screensaver_mode = false;
boolean screenvisible = true;
float mouse_acceleration = 2.0f;
int mouse_threshold = 10;
int usegamma = 0;
int usemouse = 0;

static uint16_t palette565[256];
static uint8_t palette_rgb[256 * 3];

void I_InitGraphics(void)
{
	extern void I_InitInput(void);
	memset(screen_8bit, 0, sizeof(screen_8bit));
	screenvisible = true;
	I_InitInput();
}

void I_ShutdownGraphics(void) {}
void I_StartFrame(void) {}
void I_UpdateNoBlit(void) {}

void I_StartTic(void)
{
	extern void I_GetEvent(void);
	I_GetEvent();
}

void I_FinishUpdate(void)
{
	perf_frame_rendered();

	uint64_t t0 = backend_cycles();
	backend_present(screen_8bit, palette565, palette_rgb);
	perf_add_present(backend_cycles() - t0);

	// Then hash it: in lock-step this is what proves the two machines drew
	// the same picture, pixel for pixel. After presenting, so that when DoomV
	// halts on a difference its window holds the whole frame in question.
	port_link_frame(screen_8bit, sizeof(screen_8bit));
}

void I_ReadScreen(byte *scr)
{
	memcpy(scr, screen_8bit, sizeof(screen_8bit));
}

void I_SetPalette(byte *palette)
{
	for (int i = 0; i < 256; i++) {
		uint8_t r = gammatable[usegamma][palette[3 * i + 0]];
		uint8_t g = gammatable[usegamma][palette[3 * i + 1]];
		uint8_t b = gammatable[usegamma][palette[3 * i + 2]];
		palette_rgb[3 * i + 0] = r;
		palette_rgb[3 * i + 1] = g;
		palette_rgb[3 * i + 2] = b;
		palette565[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
	}
}

int I_GetPaletteIndex(int r, int g, int b)
{
	int best = 0, best_diff = 0x7fffffff;
	for (int i = 0; i < 256; i++) {
		int dr = r - palette_rgb[3 * i], dg = g - palette_rgb[3 * i + 1], db = b - palette_rgb[3 * i + 2];
		int diff = dr * dr + dg * dg + db * db;
		if (diff < best_diff) { best = i; best_diff = diff; }
		if (diff == 0) break;
	}
	return best;
}

void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_SetWindowTitle(char *title) { (void)title; }
void I_GraphicsCheckCommandLine(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_CheckIsScreensaver(void) {}
