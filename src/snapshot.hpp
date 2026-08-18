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
	uint32_t x[32] = {};
	uint32_t pc = 0;
	bool halted = false;
	HistoryEntry active = {0, 0, "???"};
	HistoryEntry trace[4] = {{0, 0, "???"}, {0, 0, "???"}, {0, 0, "???"}, {0, 0, "???"}};
};
