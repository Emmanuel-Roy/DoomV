// The one POSIX call DOOM makes that newlib on bare metal lacks. There is no
// filesystem, so creating the savegame directory just fails, as do the
// fopen()s of config and savegame files -- DOOM falls back to defaults.
// (DoomV's guest build needed the same; see its libc_shim.c.)
#include <errno.h>
#include <sys/stat.h>

int mkdir(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	errno = ENOSYS;
	return -1;
}
