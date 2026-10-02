#pragma once
#include <cstdint>
#include <vector>

// The CLINT: one mtime for the machine, and per hart an msip and an mtimecmp
// (MTIP = mtime >= that hart's mtimecmp, MSIP = bit 0 of its msip). The layout
// is Sail's (model/sys/platform.sail): msip at 4*hart, mtimecmp at
// 0x4000 + 8*hart, mtime at 0xBFF8, each as 32-bit halves so Memory's
// read32/write32 -- and the read64/write64 that compose from them -- reach
// them without any extra plumbing.
//
// mtime ticks once every two steps and on each tick of a wait, as Sail's
// clock does (see DoomSystem::clock_tick), not wall-clock host time --
// deterministic, so a hand-written test can compute an exact fire time.
//
// The getters without a hart number answer for the current hart -- the one
// DoomSystem is stepping, set with select() -- which is the hart the CSR and
// interrupt logic always means.
class Timer {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr uint64_t MSIP_OFF     = 0x0000;
	static constexpr uint64_t MTIMECMP_OFF = 0x4000;
	static constexpr uint64_t MTIME_OFF    = 0xBFF8;

	Timer();

	// How many harts the CLINT serves. Set before anything runs.
	void set_harts(unsigned n);
	unsigned harts() const { return (unsigned)cmp.size(); }
	void select(unsigned hart) { cur = hart; }

	void tick(uint32_t count);

	uint64_t get_mtime() const { return mtime; }
	uint64_t get_mtimecmp() const { return cmp[cur]; }
	uint64_t cmp_generation() const { return cmp_gen; }
	bool mtip_pending() const { return mtime >= cmp[cur]; }
	bool msip_pending() const { return (msip[cur] & 1) != 0; }

	uint32_t read32(uint64_t offset) const;
	void write32(uint64_t offset, uint32_t val);

private:
	uint64_t mtime;
	std::vector<uint64_t> cmp;    // mtimecmp, per hart
	std::vector<uint32_t> msip;   // per hart; bit 0 is the only one there is
	unsigned cur = 0;
	uint64_t cmp_gen = 0;         // bumped by every write here
};
