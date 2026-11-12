// Replaces doomgeneric.c, and is where every target starts DOOM.
//
// The only change from doomgeneric_Create is the missing
// malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4): the 32-bit screen copy
// is not used (see port_video.c), so DG_ScreenBuffer stays NULL.

#include <string.h>

#include "doomgeneric.h"
#include "m_argv.h"
#include "port.h"
#include "port_link.h"

pixel_t *DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain(void);
void port_setup_controls(void);
void port_set_zone(uint8_t *zone, int size);

void doomgeneric_Create(int argc, char **argv)
{
	myargc = argc;
	myargv = argv;
	M_FindResponseFile();
	DG_Init();
	D_DoomMain();
}

// Never returns. `extra` are additional DOOM arguments (e.g. "-timedemo",
// "demo1" for the benchmark), NULL-terminated.
void port_run(char **extra)
{
	static char *argv[16] = { "doom", "-iwad", "doom1.wad" };
	int argc = 3;
	while (extra && *extra && argc < 15) argv[argc++] = *extra++;
	argv[argc] = NULL;

	// The zone is taken before DOOM starts, so its exact size can open the
	// lock-step recording: the replay must give DOOM an identical heap, or
	// allocations (and so cache purges and the screen melt) could differ.
	uint32_t wad_size;
	int zone_size;
	backend_wad(&wad_size);
	port_set_zone(backend_zone(&zone_size), zone_size);
	char args[256] = "";
	for (int i = 3; i < argc; i++) {
		if (i > 3) strncat(args, " ", sizeof args - strlen(args) - 1);
		strncat(args, argv[i], sizeof args - strlen(args) - 1);
	}
	port_link_start(wad_size, (uint32_t)zone_size, args);

	// D_DoomMain loads the config (there is none, so: defaults) and then
	// runs the first frame; the controls have to be bound before any input
	// is read, and m_controls' defaults are plain variables, so set them now.
	port_setup_controls();
	doomgeneric_Create(argc, argv);

	for (;;) doomgeneric_Tick();
}
