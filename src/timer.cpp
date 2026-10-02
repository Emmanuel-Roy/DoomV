#include "timer.hpp"
#include "event_gen.hpp"

// mtimecmp resets to 0 -- MTIP would be immediately pending at boot if
// left there, but every mtimecmp write always happens before mie.MTIE is
// ever set by anything running on this project so far, so this matches
// real hardware's own reset behavior without needing a "not yet armed"
// sentinel.
Timer::Timer() : mtime(0), cmp(1, 0), msip(1, 0)
{
}

void Timer::set_harts(unsigned n)
{
	cmp.assign(n, 0);
	msip.assign(n, 0);
	cur = 0;
}

void Timer::tick(uint32_t count)
{
	mtime += count;
}

uint32_t Timer::read32(uint64_t offset) const
{
	const uint64_t n = cmp.size();
	if (offset < MTIMECMP_OFF) {
		if (offset % 4 == 0 && offset / 4 < n) return msip[offset / 4];
		return 0;
	}
	if (offset < MTIME_OFF) {
		const uint64_t h = (offset - MTIMECMP_OFF) / 8;
		if (h >= n) return 0;
		return (offset & 4) ? (uint32_t)(cmp[h] >> 32) : (uint32_t)(cmp[h] & 0xFFFFFFFFu);
	}
	switch (offset) {
	case MTIME_OFF:     return (uint32_t)(mtime & 0xFFFFFFFFu);
	case MTIME_OFF + 4: return (uint32_t)(mtime >> 32);
	default:            return 0;
	}
}

// Every write can change whether some hart has an interrupt pending -- and
// a write to mtime can move it backwards, which DoomSystem's interrupt check
// otherwise assumes it never does -- so each one bumps the event generation.
void Timer::write32(uint64_t offset, uint32_t val)
{
	const uint64_t n = cmp.size();
	if (offset < MTIMECMP_OFF) {
		if (offset % 4 != 0 || offset / 4 >= n) return;
		msip[offset / 4] = val & 1;
	} else if (offset < MTIME_OFF) {
		const uint64_t h = (offset - MTIMECMP_OFF) / 8;
		if (h >= n || offset % 4 != 0) return;
		uint64_t &c = cmp[h];
		c = (offset & 4) ? (c & 0x00000000FFFFFFFFull) | ((uint64_t)val << 32)
		                 : (c & 0xFFFFFFFF00000000ull) | val;
	} else if (offset == MTIME_OFF) {
		mtime = (mtime & 0xFFFFFFFF00000000ull) | val;
	} else if (offset == MTIME_OFF + 4) {
		mtime = (mtime & 0x00000000FFFFFFFFull) | ((uint64_t)val << 32);
	} else {
		return;
	}
	cmp_gen++;
	bump_event_gen();
}
