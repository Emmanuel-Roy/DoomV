// Replaces doomgeneric's w_file.c / w_file_stdc.c.
//
// There is no filesystem: the packed IWAD sits in flash (on the Pico) or in
// guest memory (DoomV), and the backend hands us a pointer to it. Setting
// wad_file_t.mapped is the important part -- with it, W_CacheLumpNum()
// returns a pointer straight into the WAD instead of copying the lump into
// the zone, so textures, sprites, flats and the like cost no RAM at all.
// (Same trick as DoomV's w_file_doomv.c.)

#include <string.h>
#include <strings.h>

#include "doomtype.h"
#include "w_file.h"
#include "port.h"

static wad_file_t the_wad;

wad_file_t *W_OpenFile(char *path)
{
	// Only the IWAD exists. Anything else -- e.g. "demo1.lmp", which
	// -playdemo/-timedemo try first before falling back to the DEMO1 lump --
	// must fail, or DOOM would load the whole IWAD again as that file.
	if (strcasecmp(path, "doom1.wad") != 0) return NULL;
	uint32_t size;
	the_wad.file_class = NULL;
	the_wad.mapped = (byte *)backend_wad(&size);
	the_wad.length = size;
	return &the_wad;
}

void W_CloseFile(wad_file_t *wad)
{
	(void)wad;
}

size_t W_Read(wad_file_t *wad, unsigned int offset, void *buffer, size_t buffer_len)
{
	if (offset >= wad->length) return 0;
	if (offset + buffer_len > wad->length) buffer_len = wad->length - offset;
	memcpy(buffer, wad->mapped + offset, buffer_len);
	return buffer_len;
}
