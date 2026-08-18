#include <stdint.h>

#include "doomv_mmio.h"
#include "doomgeneric.h"

void DG_Init(void)
{
	// Host GUI window already exists before this binary even starts.
}

void DG_DrawFrame(void)
{
	volatile uint32_t *fb = (volatile uint32_t *)MMIO_FB;
	for (int i = 0; i < DOOMGENERIC_RESX * DOOMGENERIC_RESY; i++) {
		fb[i] = DG_ScreenBuffer[i];
	}
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
	static char *argv[] = { "doomv", "-iwad", "doom1.wad" };
	int argc = 3;

	doomgeneric_Create(argc, argv);

	while (1) {
		doomgeneric_Tick();
	}

	return 0;
}
