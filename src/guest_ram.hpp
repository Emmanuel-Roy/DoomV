#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// The guest's RAM, and how it is backed.
//
// It is one flat allocation of a gigabyte or more, written once at load and
// then read and written at scattered addresses for the rest of the run, which
// makes it exactly the kind of buffer where how it was allocated shows up in
// how fast it is: how many TLB entries it needs, whether the pages are faulted
// in up front or on first touch, and whether the OS will back it with large
// pages.
//
// Three backends, chosen at run time so they can be measured against each
// other in one binary rather than one build each:
//
//   Vector     std::vector<uint8_t>. What this used to be, kept as the
//              reference. The allocator zeroes it, which touches every page
//              before the guest starts.
//   Pages      The OS's own virtual memory, VirtualAlloc or mmap. Pages
//              arrive zeroed and on first touch, so allocation is free and
//              the cost moves into the run.
//   HugePages  As Pages, but asking for large pages -- 2MB instead of 4KB,
//              so a gigabyte needs ~512 TLB entries instead of ~262,000.
//              This is the one that can actually fail: Windows wants
//              SeLockMemoryPrivilege, which a normal account usually does not
//              have, and Linux wants transparent hugepages enabled. When it
//              cannot be had, allocate() falls back to Pages and says so
//              through active(), rather than pretending.
class GuestRam {
public:
	enum class Backend { Vector, Pages, HugePages };

	GuestRam() = default;
	~GuestRam();
	GuestRam(const GuestRam &) = delete;
	GuestRam &operator=(const GuestRam &) = delete;

	// Zeroed, `bytes` long. Throws std::bad_alloc if nothing can supply it.
	void allocate(size_t bytes, Backend wanted);

	uint8_t *data() { return ptr; }
	const uint8_t *data() const { return ptr; }
	size_t size() const { return length; }
	uint8_t &operator[](size_t i) { return ptr[i]; }
	const uint8_t &operator[](size_t i) const { return ptr[i]; }

	// What was actually used, which is not always what was asked for.
	Backend active() const { return active_backend; }
	static const char *name(Backend b);
	// DOOMV_RAM_BACKEND=vector|pages|hugepages, defaulting to the build's
	// default when unset or unrecognised. An environment variable rather
	// than a flag because this exists to be A/B'd by performance/bench.py,
	// which runs the emulator's own command line.
	static Backend from_env();

private:
	void release();

	uint8_t *ptr = nullptr;
	size_t length = 0;
	Backend active_backend = Backend::Vector;
	std::vector<uint8_t> heap;   // holds the storage for Backend::Vector only
};
