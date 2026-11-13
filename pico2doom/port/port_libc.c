// One C-library layer for both RISC-V builds -- the Pico firmware and the
// DoomV guest -- so DOOM executes the same instructions on both, down to the
// library (docs/LOCKSTEP.md, "identical builds").
//
// Both link the same newlib and libgcc from the same toolchain. What used to
// differ was the layer around them:
//
//   * pico-sdk wraps malloc/calloc/realloc/free (with a mutex) and
//     printf/vprintf/puts (through its stdio drivers), by --wrap link options
//     its runtime always adds. On a real Pico these ran inside DOOM's counted
//     windows and retired different instructions than DoomV's plain calls --
//     the 64-instruction difference at E1M5's level load.
//   * the system calls (_sbrk, _write, ...) came from pico-sdk on the Pico and
//     from a hand-written shim on DoomV.
//
// Now the shared code never calls the wrapped names: port.cmake renames them
// (malloc -> port_malloc, ...) in every engine and port file on the RISC-V
// targets, and they are implemented here on newlib's reentrant entry points,
// which nothing wraps. The system calls are defined here too (pico-sdk's are
// weak). Where a call has to reach the hardware -- console output -- the
// backend's part runs in an excluded window, as DG_GetTicksMs's does.

#include <errno.h>
#include <reent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>

#include "port.h"
#include "port_instret.h"
#include "port_link.h"

// ---- memory ------------------------------------------------------------------

void *port_malloc(size_t n) { return _malloc_r(_REENT, n); }
void *port_calloc(size_t n, size_t size) { return _calloc_r(_REENT, n, size); }
void *port_realloc(void *p, size_t n) { return _realloc_r(_REENT, p, n); }
void port_free(void *p) { _free_r(_REENT, p); }

// ---- memset / memmove ------------------------------------------------------------
//
// newlib's RISC-V memset (and memmove) aligns to 16 bytes before its
// unrolled loop, so the instructions it retires depend on the destination
// address mod 16. Addresses agree mod 4 between the Pico and DoomV images
// (statics, zone, heap, stack and WAD are all word-aligned the same way)
// but not mod 16 -- so the same memset retired different counts on the two
// machines. These replacements look only at an address mod 4. memcpy and
// newlib's string functions already do.
//
// The loop-pattern optimisation is off for them: GCC would otherwise turn
// the byte loops back into calls to memset itself.

#define NOPATTERN __attribute__((optimize("no-tree-loop-distribute-patterns")))

NOPATTERN void *memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	uint8_t b = (uint8_t)c;
	while (n && ((uintptr_t)d & 3)) { *d++ = b; n--; }
	uint32_t w = 0x01010101u * b;
	uint32_t *dw = (uint32_t *)d;
	for (; n >= 16; n -= 16, dw += 4) { dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w; }
	for (; n >= 4; n -= 4) *dw++ = w;
	d = (uint8_t *)dw;
	while (n--) *d++ = b;
	return dst;
}

NOPATTERN void *memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	// Copy backwards only when dst really overlaps the end of src -- never
	// by comparing unrelated addresses, whose order differs between images.
	if ((uintptr_t)d - (uintptr_t)s < n) {
		d += n; s += n;
		while (n--) *--d = *--s;
		return dst;
	}
	if ((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {     // same alignment: words
		while (n && ((uintptr_t)d & 3)) { *d++ = *s++; n--; }
		uint32_t *dw = (uint32_t *)d;
		const uint32_t *sw = (const uint32_t *)s;
		for (; n >= 4; n -= 4) *dw++ = *sw++;
		d = (uint8_t *)dw; s = (const uint8_t *)sw;
	}
	while (n--) *d++ = *s++;
	return dst;
}

// ---- output --------------------------------------------------------------------

int port_vprintf(const char *fmt, va_list ap) { return _vfprintf_r(_REENT, _stdout_r(_REENT), fmt, ap); }

int port_printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = port_vprintf(fmt, ap);
	va_end(ap);
	return n;
}

int port_puts(const char *s) { return _puts_r(_REENT, s); }

// ---- system calls ------------------------------------------------------------------

// The heap newlib's malloc grows into: [heap_lo, heap_hi), from the backend.
static char *heap_lo, *heap_hi, *heap_brk;

void *_sbrk(ptrdiff_t incr)
{
	if (!heap_brk) {
		backend_heap((void **)&heap_lo, (void **)&heap_hi);
		heap_brk = heap_lo;
	}
	if (incr > heap_hi - heap_brk) {
		errno = ENOMEM;
		return (void *)-1;
	}
	char *prev = heap_brk;
	heap_brk += incr;
	// Zeroed, like the zone (port_main.c): memory read before it is written
	// must hold the same bytes on the Pico and on DoomV.
	if (incr > 0) memset(prev, 0, (size_t)incr);
	return prev;
}

// stdout and stderr: the backend's console, measured out of the counted
// windows like the engine's other platform calls.
int _write(int fd, const char *buf, int len)
{
	(void)fd;
	uint32_t i0 = port_instret();
	backend_console(buf, len);
	port_link_exclude_instret(port_instret() - i0);
	return len;
}

// No filesystem: DOOM's config and savegame files simply do not open.
int _open(const char *name, int flags, ...) { (void)name; (void)flags; errno = ENOENT; return -1; }
int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; errno = EBADF; return -1; }
off_t _lseek(int fd, off_t pos, int whence) { (void)fd; (void)pos; (void)whence; errno = EBADF; return -1; }
int _unlink(const char *name) { (void)name; errno = ENOENT; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = ENOENT; return -1; }
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }

int _fstat(int fd, struct stat *st)
{
	memset(st, 0, sizeof *st);
	if (fd > 2) { errno = EBADF; return -1; }
	st->st_mode = S_IFCHR;           // the console: unbuffered character device
	return 0;
}

int _isatty(int fd) { return fd <= 2; }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _gettimeofday(struct timeval *tv, void *tz) { (void)tz; tv->tv_sec = 0; tv->tv_usec = 0; return 0; }
clock_t _times(struct tms *t) { memset(t, 0, sizeof *t); return 0; }

void _exit(int status)
{
	(void)status;
	backend_fatal("exit() called");
}

// assert(): the same on both builds (pico-sdk's is weak).
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
	static char msg[160];
	snprintf(msg, sizeof msg, "assertion \"%s\" failed: %s:%d (%s)", expr, file, line, func ? func : "");
	backend_fatal(msg);
}

// Set the heap up before DOOM starts (port_run), so the backend's part of
// _sbrk never runs inside a counted window on either machine.
void port_libc_init(void) { _sbrk(0); }
