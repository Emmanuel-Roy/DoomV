#pragma once
#include <cstdint>

// CLINT-style timer: mtime is a free-running 64-bit counter, mtimecmp is
// the M-mode-facing comparator (MTIP = mtime >= mtimecmp). Registers are
// exposed at their real CLINT byte offsets (mtimecmp at 0x4000, mtime at
// 0xBFF8, each as two 32-bit halves 4 bytes apart) so Memory's existing
// read32/write32 -- and the read64/write64 that already compose from
// them -- both work without any extra plumbing.
//
// mtime ticks from instructions retired (see Memory::step_instructions),
// not wall-clock host time -- deterministic, so a hand-written test can
// compute an exact fire time. Real wall-clock timing would make this as
// unreproducible as the existing MMIO_TICK/tick_counter pacing hack,
// which is explicitly not a real timer.
class Timer {
public:
	static constexpr uint64_t MTIMECMP_OFF = 0x4000;
	static constexpr uint64_t MTIME_OFF    = 0xBFF8;

	Timer();

	void tick(uint32_t count);

	uint64_t get_mtime() const { return mtime; }
	uint64_t get_mtimecmp() const { return mtimecmp; }
	bool mtip_pending() const { return mtime >= mtimecmp; }

	uint32_t read32(uint64_t offset) const;
	void write32(uint64_t offset, uint32_t val);

private:
	uint64_t mtime;
	uint64_t mtimecmp;
};
