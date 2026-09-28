#include <stdint.h>

#include "doomv_mmio.h"
#include "doomgeneric.h"
#include "d_event.h"

void DG_Init(void)
{
	// Host GUI window already exists before this binary even starts.
}

// The mouse, posted straight into DOOM's event queue rather than coming
// back through DG_GetKey.
//
// doomgeneric's generic input hook is DG_GetKey, which carries a key code
// and a pressed flag and has no way to express two axes of movement -- the
// mouse block in its i_input.c is commented out for exactly that reason.
// DOOM itself has the whole path already: g_game.c's ev_mouse case sets
// mousex/mousey and G_BuildTiccmd turns those into a tic command. So all
// that is missing is something to post the event, and a port is allowed to
// do that directly. Doing it here also avoids touching doomgeneric, which
// is a pinned submodule.
//
// Once per frame, from DG_DrawFrame, because that is where a real port
// reads its mouse (I_ReadMouse, called from I_FinishUpdate) and because
// MMIO_MOUSE_MOVE is destructive: reading it twice in a frame would report
// half the movement each time.
static void doomv_post_mouse(void)
{
	static uint32_t last_buttons = 0;

	uint32_t move = *(volatile uint32_t *)MMIO_MOUSE_MOVE;
	uint32_t buttons = *(volatile uint32_t *)MMIO_MOUSE_BTN;

	int dx = (int)(short)(move >> 16);
	int dy = (int)(short)(move & 0xFFFFu);

	// Nothing happened: no event. Posting a zero-movement event every
	// frame would work, but it would also fill the event queue with
	// nothing and make the menu's mouse handling jumpy.
	if (dx == 0 && dy == 0 && buttons == last_buttons) {
		return;
	}
	last_buttons = buttons;

	event_t ev;
	ev.type = ev_mouse;
	ev.data1 = (int)buttons;
	ev.data2 = dx;
	// Negated: the window's Y axis grows downward and DOOM's forward axis
	// grows up, so passing it through unchanged gives a mouse that walks
	// backwards. Every port does this.
	ev.data3 = -dy;
	ev.data4 = 0;
	D_PostEvent(&ev);
}

void DG_DrawFrame(void)
{
	volatile uint32_t *fb = (volatile uint32_t *)MMIO_FB;
	for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) {
		fb[i] = DG_ScreenBuffer[i];
	}

	doomv_post_mouse();
}

uint32_t DG_GetTicksMs(void)
{
	return *(volatile uint32_t *)MMIO_TICK;
}

void DG_SleepMs(uint32_t ms)
{
	uint32_t start = DG_GetTicksMs();
	while (DG_GetTicksMs() - start < ms) { }
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
	uint32_t val = *(volatile uint32_t *)MMIO_INPUT;
	if (val == 0) {
		return 0;
	}

	*pressed = (val >> 8) & 0xFF;
	*doomKey = val & 0xFF;
	return 1;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}

int main(void)
{
	static char *argv[] = { "doomv", "-iwad", IWAD_NAME };
	int argc = 3;

	doomgeneric_Create(argc, argv);

	while (1) {
		doomgeneric_Tick();
	}

	return 0;
}
