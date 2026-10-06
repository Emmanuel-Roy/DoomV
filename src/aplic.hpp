#pragma once
#include <cstdint>
#include <vector>

class Imsic;

// A single APLIC domain, 32 interrupt sources (1..31 --
// index 0 is unused/reserved, matching the spec's own 1-based source
// numbering). Byte offsets are confirmed against the ratified RISC-V AIA
// 1.0 spec §4.5's memory-map table -- see the comments in aplic.cpp.
//
// Real AIA ties an entire domain to one privilege level (M or S), with
// per-hart routing handled by the hart-index field in each source's
// target register, not by the domain itself. So the design choice is which
// single domain to model -- this one forwards to the S-level IMSIC files,
// each source to the hart its target names, matching how a real
// OpenSBI+Linux platform hands peripheral interrupts to the kernel
// rather than firmware.
class Aplic {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr int NUM_SOURCES = 32; // indices 1..31 used

	explicit Aplic(std::vector<Imsic> &s_files);

	uint32_t read32(uint64_t offset) const;
	void write32(uint64_t offset, uint32_t val);

	// A device asserting its interrupt line. Until virtio there were no
	// such devices here -- the UART is polled and the timer goes through
	// the CLINT -- so the only way a source ever became pending was a
	// guest writing setipnum, which is the software-triggered path. A real
	// peripheral does not write its own controller; it raises a wire, and
	// this is that wire. The forwarding is identical, and deliberately
	// shares the same implementation, because a source the domain has not
	// configured or enabled must be dropped just as silently either way.
	void assert_source(uint32_t source);

	// Direct delivery (domaincfg.DM = 0): no IMSIC, no AIA CSRs. Sources
	// are pending and enabled here, each hart has an interrupt delivery
	// control block (IDC) at 0x4000 + 32 * hart, and what reaches the hart
	// is one wire, its SEIP. This is the APLIC a machine without Smaia/Ssaia
	// has -- Sail's, for one: the Linux machine lock-stepped against Sail
	// (tools/linux/dts/doomv-sail.dts) uses it.
	bool direct_line(unsigned hart) const;
	static constexpr uint64_t IDC_BASE = 0x4000;

private:
	std::vector<Imsic> &s_files;   // each hart's S-level file

	bool direct() const { return !(domaincfg & (1u << 2)); }
	// The highest-priority pending, enabled source targeting `hart` and
	// above its threshold, as topi encodes it: id << 16 | priority, or 0.
	uint32_t topi(unsigned hart) const;

	uint32_t domaincfg;
	uint32_t sourcecfg[NUM_SOURCES]; // [0] unused
	uint32_t target[NUM_SOURCES];    // [0] unused
	// Direct mode: pending and enabled, a bit per source (bit 0 unused), and
	// each hart's IDC.
	uint32_t ip = 0, ie = 0;
	struct Idc { uint32_t idelivery = 0, iforce = 0, ithreshold = 0; };
	std::vector<Idc> idc;
};
