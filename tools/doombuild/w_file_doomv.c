// Replaces doomgeneric's w_file.c for this build -- not linked, see the
// guest Makefile. There's exactly one embedded WAD, sitting in guest RAM
// at WAD_BASE for the whole run, so this skips the wad_file_class_t
// indirection (stdc/mmap backend selection) entirely.

#include <stddef.h>

#include "doomv_mmio.h"
#include "doomtype.h"
#include "w_file.h"

#ifndef WAD_LENGTH
#error "WAD_LENGTH must be defined by the build (see guest Makefile)"
#endif

wad_file_t *W_OpenFile(char *path)
{
	(void)path; // no real filesystem to search, always the embedded WAD

	static wad_file_t wad;
	wad.file_class = NULL;
	wad.mapped = (byte *)WAD_BASE;
	wad.length = WAD_LENGTH;

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
