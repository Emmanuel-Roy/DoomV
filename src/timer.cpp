#include "timer.hpp"

// mtimecmp resets to 0 -- MTIP would be immediately pending at boot if
// left there, but every mtimecmp write always happens before mie.MTIE is
// ever set by anything running on this project so far, so this matches
// real hardware's own reset behavior without needing a "not yet armed"
// sentinel.
Timer::Timer() : mtime(0), mtimecmp(0)
{
}

void Timer::tick(uint32_t count)
{
	mtime += count;
}

uint32_t Timer::read32(uint64_t offset) const
{
	switch (offset) {
	case MTIMECMP_OFF:     return (uint32_t)(mtimecmp & 0xFFFFFFFFu);
	case MTIMECMP_OFF + 4: return (uint32_t)(mtimecmp >> 32);
	case MTIME_OFF:        return (uint32_t)(mtime & 0xFFFFFFFFu);
	case MTIME_OFF + 4:    return (uint32_t)(mtime >> 32);
	default:               return 0;
	}
}

void Timer::write32(uint64_t offset, uint32_t val)
{
	// mtime itself is read-only here -- nothing in this project needs to
	// step it backward/forward by hand, only mtimecmp is ever armed.
	switch (offset) {
	case MTIMECMP_OFF:     mtimecmp = (mtimecmp & 0xFFFFFFFF00000000ull) | val; break;
	case MTIMECMP_OFF + 4: mtimecmp = (mtimecmp & 0x00000000FFFFFFFFull) | ((uint64_t)val << 32); break;
	default: break;
	}
}
