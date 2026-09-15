#pragma once
#include <cstdint>

// One IMSIC interrupt file (there are two instances in this project: the
// M-level file and the S-level file, both single-hart since only one
// hart exists). Register selectors (eidelivery/eithreshold/eip*/eie*)
// and CSR addresses (miselect/mireg for M, siselect/sireg for S) are
// confirmed against the ratified RISC-V AIA 1.0 spec, Table 2/§2.3 and
// Table 3/Table 4 -- see the comments at each call site in ext_zicsr.cpp.
//
// eip/eie are 2048-bit arrays (64 x 32-bit words) per spec, but nothing
// in this project will ever use interrupt identities anywhere near that
// range -- kept at full width anyway since it costs nothing and avoids
// a second, narrower convention to remember.
class Imsic {
public:
	static constexpr int NUM_WORDS = 64;

	Imsic();

	// miselect/siselect-indexed indirect register window (mireg/sireg).
	uint64_t read_indirect(uint64_t iselect) const;
	void write_indirect(uint64_t iselect, uint64_t value);

	// mtopei/stopei: peek without side effects (used for the CSR's "old"
	// value in a plain CSRRS x0 read) vs. claim (used when a real write
	// happens, per spec -- any write value is ignored, the act of
	// writing clears whatever was on top). Returns (id<<16)|id, or 0 if
	// nothing is currently pending+enabled+unmasked by eithreshold.
	uint32_t topei_value() const;
	void claim();

	// The MMIO trigger a device (or, here, Aplic) writes to raise
	// interrupt `id` -- mirrors real seteipnum_le (AIA spec §3.5).
	void set_pending(uint32_t id);

	bool delivery_enabled() const { return eidelivery; }
	bool aggregate_pending() const { return top != 0; }

	// Bumped by every change to the file's state, so a caller that decided
	// "nothing pending" can tell when that decision has gone stale.
	uint64_t generation() const { return gen; }

private:
	// topei, recomputed whenever eidelivery, eithreshold, eip or eie
	// changes -- every one of which happens in a member below. It used to
	// be rescanned, 64 words at a time, on every read, and compute_mip reads
	// it before every instruction: a quarter of a Linux boot's host time.
	uint32_t top = 0;
	uint64_t gen = 0;
	void refresh();

	bool eidelivery;
	uint32_t eithreshold;
	uint32_t eip[NUM_WORDS];
	uint32_t eie[NUM_WORDS];

	bool heard(uint32_t id) const;
};
