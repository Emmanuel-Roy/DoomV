// Replaces doomgeneric's w_file.c for this build -- not linked, see the
// guest Makefile. There's exactly one embedded WAD, sitting in guest RAM
// at WAD_BASE for the whole run, so this skips the wad_file_class_t
// indirection (stdc/mmap backend selection) entirely.

#include <stddef.h>
#include <stdint.h>

#include "doomv_mmio.h"
#include "doomtype.h"
#include "w_file.h"

wad_file_t *W_OpenFile(char *path)
{
	(void)path; // no real filesystem to search, always the embedded WAD

	// Both come from the host, which is the only side that knows either.
	// This used to be a hardcoded WAD_BASE plus a WAD_LENGTH baked in by
	// the Makefile at build time; the address drifted out of step with
	// src/memory.hpp and DOOM stopped finding its WAD. Asking cannot
	// drift, and taking the length from the host as well means this
	// binary runs whichever WAD it is handed rather than only the one it
	// was compiled beside.
	static wad_file_t wad;
	wad.file_class = NULL;
	wad.mapped = (byte *)(uintptr_t)doomv_wad_base();
	wad.length = (int)doomv_wad_size();

	return &wad;
}

void W_CloseFile(wad_file_t *wad)
{
	(void)wad; // nothing to release, the WAD lives in guest RAM the whole run
}

size_t W_Read(wad_file_t *wad, unsigned int offset, void *buffer, size_t buffer_len)
{
	if (offset >= wad->length) {
		return 0;
	}
	if (offset + buffer_len > wad->length) {
		buffer_len = wad->length - offset;
	}

	byte *src = wad->mapped + offset;
	byte *dst = (byte *)buffer;
	for (size_t i = 0; i < buffer_len; i++) {
		dst[i] = src[i];
	}

	return buffer_len;
}
