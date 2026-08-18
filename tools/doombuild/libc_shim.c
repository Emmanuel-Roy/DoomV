// Small gaps in newlib's nosys.specs stub set -- no real filesystem here,
// so these just fail the way the rest of nosys's syscalls already do.

#include <errno.h>

int mkdir(const char *path, int mode)
{
	(void)path;
	(void)mode;
	errno = ENOSYS;
	return -1;
}
