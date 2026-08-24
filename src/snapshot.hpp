#pragma once
#include "memory.hpp"
#include "registers.hpp"
#include <cstdint>
#include <vector>

// Everything the render thread needs to draw one frame, copied out of the
// CPU thread's live state under a lock. The render thread never touches
// Memory/Registers/Debugger directly -- those are only ever mutated by the
// CPU thread, which keeps the whole rest of the emulator single-threaded
// and avoids needing locks scattered through it.
struct Snapshot {
	std::vector<uint32_t> framebuffer = std::vector<uint32_t>(Memory::FB_W * Memory::FB_H, 0);
	uint64_t x[32] = {};
	// Just the low 64 bits of each 128-bit V register -- plenty for a
	// dashboard display (they're all zero until V is actually implemented
	// anyway), not worth snapshotting the full width yet.
	uint64_t v_lo[32] = {};
	uint64_t pc = 0;
	bool halted = false;
	// DecodedInstruction default-initializes its own mnemonic to "???"
	// (see riscv_decoder.hpp), so value-initializing HistoryEntry here is
	// already a safe, displayable default -- no need to spell it out.
	HistoryEntry active{};
	HistoryEntry trace[4]{};
};
