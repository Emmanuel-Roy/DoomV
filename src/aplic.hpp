#pragma once
#include <cstdint>

class Imsic;

// A single MSI-mode-only APLIC domain, 32 interrupt sources (1..31 --
// index 0 is unused/reserved, matching the spec's own 1-based source
// numbering). Byte offsets are confirmed against the ratified RISC-V AIA
// 1.0 spec §4.5's memory-map table -- see the comments in aplic.cpp.
//
// Real AIA ties an entire domain to one privilege level (M or S), with
// per-hart routing handled by the hart-index field in each source's
// target register, not by the domain itself. This project only has one
// hart, so the only real design choice is which single domain to model
// -- this one forwards to the S-level IMSIC file, matching how a real
// OpenSBI+Linux platform hands peripheral interrupts to the kernel
// rather than firmware.
class Aplic {
public:
	static constexpr int NUM_SOURCES = 32; // indices 1..31 used

	explicit Aplic(Imsic &s_file);

	uint32_t read32(uint64_t offset) const;
	void write32(uint64_t offset, uint32_t val);

private:
	Imsic &s_file;

	uint32_t domaincfg;
	uint32_t sourcecfg[NUM_SOURCES]; // [0] unused
	uint32_t target[NUM_SOURCES];    // [0] unused
};
