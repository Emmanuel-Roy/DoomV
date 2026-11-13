// The handful of engine functions this port replaces.
//
// port.cmake renames them at compile time (PORT_WRAPS): every engine call to
// NAME goes to __wrap_NAME here, and the original is still there as
// __real_NAME -- what the linker's --wrap does, but with the preprocessor,
// so it works with any toolchain. It lets the port change what these do
// without editing the engine:
//
//   I_ZoneBase    DOOM's heap is whatever the backend can spare, not 6 MB.
//   I_Error       show the error on the LCD / console, then stop.
//   I_Quit        "quit" from the menu: stop cleanly.
//   M_FileExists  the IWAD search: the one WAD we have "exists".
//   G_DeferedPlayDemo  skip attract-mode demos whose map does not fit.
//
// Also here: the controls a few breadboard buttons need (always run,
// a next-weapon key) and the memory statistics perf reports.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "m_controls.h"
#include "w_wad.h"
#include "z_zone.h"

// The shareware maps that fit in a Pico 2's 271 KB zone with at least 8 KB
// to spare, measured with the sim (docs/MEMORY.md). Part of the shared port
// rather than the firmware: a DoomV replay has to make the same choices
// (which attract-mode demos to skip) as the Pico it replays.
#define PORT_MAPS_THAT_FIT "E1M1, E1M4, E1M5, E1M8, E1M9"

static boolean map_fits(int episode, int map)
{
	char name[8];
	snprintf(name, sizeof name, "E%dM%d", episode, map);
	return strstr(PORT_MAPS_THAT_FIT, name) != NULL;
}

#include "port.h"
#include "port_keys.h"

// The IWAD name doomgeneric_Create passes with -iwad (port_main.c).
#define PORT_IWAD_NAME "doom1.wad"

static int zone_size;
static byte *zone_base;

// port_run takes the zone from the backend before DOOM starts (port_main.c).
void port_set_zone(uint8_t *zone, int size)
{
	zone_base = zone;
	zone_size = size;
}

byte *__wrap_I_ZoneBase(int *size)
{
	*size = zone_size;
	char line[64];
	snprintf(line, sizeof line, "zone: %d KB for DOOM's heap", zone_size / 1024);
	backend_log(line);
	return zone_base;
}

void __wrap_I_Error(char *error, ...)
{
	static char msg[256];
	va_list ap;
	va_start(ap, error);
	vsnprintf(msg, sizeof msg, error, ap);
	va_end(ap);
	// The zone is full: on a Pico 2 that means this map is too big for
	// 520 KB of RAM. Say so plainly (docs/MEMORY.md has the numbers).
	if (strncmp(msg, "Z_Malloc: failed", 16) == 0) {
		static char oom[256];
		snprintf(oom, sizeof oom,
		         "Not enough RAM for E%dM%d.\n\n"
		         "The Pico 2 has 520 KB; this map needs more than the %d KB\n"
		         "left for DOOM's heap. Maps that fit: " PORT_MAPS_THAT_FIT ".\n"
		         "(%s)", gameepisode, gamemap, zone_size / 1024, msg);
		backend_fatal(oom);
	}
	backend_fatal(msg);
}

void __wrap_I_Quit(void)
{
	backend_fatal("Thanks for playing!");
}

// The title screen's attract loop plays DEMO1-3 (E1M5, E1M3, E1M7). A demo
// whose map does not fit is skipped -- straight on to the next page --
// instead of stopping DOOM with "not enough RAM" seconds after boot.
extern void __real_G_DeferedPlayDemo(char *name);
void D_AdvanceDemo(void);

void __wrap_G_DeferedPlayDemo(char *name)
{
	const byte *demo = W_CacheLumpName(name, PU_CACHE);
	// Vanilla 1.9 demo header: version, skill, episode, map, ...
	if (W_LumpLength(W_GetNumForName(name)) >= 4 && !map_fits(demo[2], demo[3])) {
		D_AdvanceDemo();
		return;
	}
	__real_G_DeferedPlayDemo(name);
}

extern boolean __real_M_FileExists(char *file);

boolean __wrap_M_FileExists(char *file)
{
	if (strcmp(file, PORT_IWAD_NAME) == 0) return true;
	return __real_M_FileExists(file);
}

// Called after DOOM has loaded its (default) config: bind the extra keys
// the button layer sends. A config file can't exist (no filesystem), so
// these are effectively the defaults.
void port_setup_controls(void)
{
	key_nextweapon = PORT_KEY_NEXTWEAPON;
	// Always run: joybspeed names a joystick button that cannot exist, which
	// DOOM treats as "held" (the vanilla always-run trick).
	joybspeed = 31;
}

// Bytes of the zone holding live (non-purgeable) data. DOOM's cache blocks
// (PU_CACHE and up) are counted as free because the allocator throws them
// away whenever it needs the room.
int port_zone_used(void)
{
	return zone_size ? zone_size - Z_FreeMemory() : 0;
}

int port_zone_size(void)
{
	return zone_size;
}
