// The few system calls newlib needs on DoomV (after DoomV's own libc_shim.c).
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>

#include "doomv_mmio.h"

int mkdir(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	errno = ENOSYS;
	return -1;
}

// printf and friends go to DoomV's debug console.
int _write(int file, char *ptr, int len)
{
	(void)file;
	for (int i = 0; i < len; i++) *(volatile uint8_t *)MMIO_DEBUG = (uint8_t)ptr[i];
	return len;
}

// A plain bump allocator over the rest of guest RAM.
extern char end;
void *_sbrk(int incr)
{
	static char *brk;
	if (!brk) brk = &end;
	char *prev = brk;
	brk += incr;
	return prev;
}
