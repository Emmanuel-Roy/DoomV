// -zonereport: who owns the zone. Every Z_Malloc is tagged with its caller
// (the engine's calls are renamed to __wrap_Z_Malloc by sim/CMakeLists.txt);
// at exit the zone's live blocks are walked and totalled per caller.
// This is how docs/MEMORY.md's table was made.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#include "z_zone.h"

// Mirrors z_zone.c's private layout (doomgeneric dcb7a8d).
typedef struct memblock_s {
	int size;
	void **user;
	int tag;
	int id;
	struct memblock_s *next, *prev;
} memblock_t;
typedef struct {
	int size;
	memblock_t blocklist;
	memblock_t *rover;
} memzone_t;
extern memzone_t *mainzone;

#define SLOTS 65536
static struct { void *ptr; void *caller; } slot[SLOTS];

extern void *__real_Z_Malloc(int size, int tag, void *user);

void *__wrap_Z_Malloc(int size, int tag, void *user)
{
	void *p = __real_Z_Malloc(size, tag, user);
	uintptr_t h = ((uintptr_t)p >> 3) % SLOTS;
	while (slot[h].ptr && slot[h].ptr != p) h = (h + 1) % SLOTS;
	slot[h].ptr = p;
	slot[h].caller = __builtin_return_address(0);
	return p;
}

// port/ is not renamed; its calls land here and show up as "untracked".
void *Z_Malloc(int size, int tag, void *user) { return __real_Z_Malloc(size, tag, user); }

static void *caller_of(void *p)
{
	uintptr_t h = ((uintptr_t)p >> 3) % SLOTS;
	for (int n = 0; n < SLOTS && slot[h].ptr; n++, h = (h + 1) % SLOTS)
		if (slot[h].ptr == p) return slot[h].caller;
	return NULL;
}

static void name_of(void *addr, char *out, int len)
{
#ifdef _WIN32
	static int init;
	if (!init) { SymInitialize(GetCurrentProcess(), NULL, TRUE); init = 1; }
	static union { SYMBOL_INFO info; char bytes[sizeof(SYMBOL_INFO) + 256]; } buf;
	buf.info.SizeOfStruct = sizeof(SYMBOL_INFO);
	buf.info.MaxNameLen = 255;
	DWORD64 off;
	if (addr && SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &off, &buf.info)) {
		snprintf(out, len, "%s", buf.info.Name);
		return;
	}
#endif
	snprintf(out, len, addr ? "%p" : "(port / untracked)", addr);
}

void sim_zone_report(void)
{
	struct { char name[64]; int bytes, blocks; } row[256];
	int nrows = 0, total = 0;
	for (memblock_t *b = mainzone->blocklist.next; b != &mainzone->blocklist; b = b->next) {
		if (b->tag == PU_FREE || b->tag >= PU_PURGELEVEL) continue;
		char name[64];
		name_of(caller_of((byte *)b + sizeof(memblock_t)), name, sizeof name);
		int i;
		for (i = 0; i < nrows && strcmp(row[i].name, name); i++) {}
		if (i == nrows && nrows < 256) { strcpy(row[nrows].name, name); row[nrows].bytes = row[nrows].blocks = 0; nrows++; }
		if (i < 256) { row[i].bytes += b->size; row[i].blocks++; }
		total += b->size;
	}
	for (int i = 0; i < nrows; i++)          // sort, biggest first
		for (int j = i + 1; j < nrows; j++)
			if (row[j].bytes > row[i].bytes) { __typeof__(row[0]) t = row[i]; row[i] = row[j]; row[j] = t; }
	printf("zone report: %d KB live, by allocating function (bytes include 24-byte block headers)\n", total / 1024);
	for (int i = 0; i < nrows; i++)
		printf("  %8d B %5d blocks  %s\n", row[i].bytes, row[i].blocks, row[i].name);
}
