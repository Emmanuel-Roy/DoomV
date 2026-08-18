// Small gaps in newlib's nosys.specs stub set -- no real filesystem here,
// so most of these just fail the way the rest of nosys's syscalls already do.

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "doomv_mmio.h"

int mkdir(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	errno = ENOSYS;
	return -1;
}

// M_FileExists (used by the IWAD search, d_iwad.c) calls fopen(), which
// goes through _open() -- not through w_file_doomv.c's W_OpenFile at all,
// that only ever gets reached once the IWAD's path has already been
// resolved. Only IWAD_NAME opened read-only succeeds, so a config file
// that genuinely doesn't exist yet still correctly falls back to defaults
// instead of every _open() call succeeding unconditionally.
#define IWAD_FD 3

static uint32_t iwad_fd_offset = 0;
static int iwad_fd_open = 0;

int _open(const char *name, int flags, ...)
{
	if ((flags & (O_WRONLY | O_RDWR | O_CREAT)) == 0 && strcmp(name, IWAD_NAME) == 0) {
		iwad_fd_offset = 0;
		iwad_fd_open = 1;
		return IWAD_FD;
	}
	errno = ENOENT;
	return -1;
}

int _read(int fd, char *buf, int len)
{
	if (fd != IWAD_FD || !iwad_fd_open) {
		errno = EBADF;
		return -1;
	}

	if (iwad_fd_offset >= WAD_LENGTH) return 0;
	if (iwad_fd_offset + (uint32_t)len > WAD_LENGTH) len = (int)(WAD_LENGTH - iwad_fd_offset);

	const volatile uint8_t *src = (const volatile uint8_t *)(WAD_BASE + iwad_fd_offset);
	for (int i = 0; i < len; i++) buf[i] = (char)src[i];
	iwad_fd_offset += (uint32_t)len;

	return len;
}

int _close(int fd)
{
	if (fd != IWAD_FD || !iwad_fd_open) {
		errno = EBADF;
		return -1;
	}
	iwad_fd_open = 0;
	return 0;
}

int _lseek(int fd, int offset, int whence)
{
	if (fd != IWAD_FD || !iwad_fd_open) {
		errno = EBADF;
		return -1;
	}

	uint32_t new_offset;
	switch (whence) {
	case SEEK_SET: new_offset = (uint32_t)offset; break;
	case SEEK_CUR: new_offset = iwad_fd_offset + (uint32_t)offset; break;
	case SEEK_END: new_offset = WAD_LENGTH + (uint32_t)offset; break;
	default: errno = EINVAL; return -1;
	}

	if (new_offset > WAD_LENGTH) { errno = EINVAL; return -1; }
	iwad_fd_offset = new_offset;
	return (int)new_offset;
}

// Overrides nosys.specs's always-fail _write -- routes stdout/stderr to
// MMIO_DEBUG instead of silently dropping printf/I_Error output.
int _write(int file, char *ptr, int len)
{
	(void)file;
	volatile uint8_t *dbg = (volatile uint8_t *)MMIO_DEBUG;
	for (int i = 0; i < len; i++) {
		*dbg = (uint8_t)ptr[i];
	}
	return len;
}

// Overrides the toolchain's own precompiled _sbrk -- it failed a 64KB
// malloc right after a 6MB one succeeded, for reasons not visible from
// outside libc.a (no source shipped, just the compiled .o). A simple bump
// allocator we control removes the guesswork; matches the same pattern
// the reference project's libc_backend.c uses. Bounded at WAD_BASE (the
// top of the RAM region, see riscv.lds) -- past that is the WAD data
// itself, growing into it would corrupt what w_file_doomv.c reads.
extern char end;

void *_sbrk(int incr)
{
	static char *heap_ptr = 0;
	if (heap_ptr == 0) heap_ptr = &end;

	char *prev = heap_ptr;
	if ((uint32_t)(heap_ptr + incr) > WAD_BASE) {
		errno = ENOMEM;
		return (void *)-1;
	}

	heap_ptr += incr;
	return prev;
}
