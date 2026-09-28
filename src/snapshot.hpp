#pragma once
#include "memory.hpp"
#include "registers.hpp"
#include <cstdint>
#include <vector>

// Everything the dashboard thread needs to draw the panels, copied out of
// the CPU thread's live state under a lock. Neither the dashboard nor the
// window thread touches Memory/Registers/Debugger directly -- those are only
// ever mutated by the CPU thread, which keeps the whole rest of the emulator
// single-threaded and avoids needing locks scattered through it.
//
// The framebuffer is not in here any more. It used to be, copied whole on
// every burst -- 4.9MB for a Linux guest, whether or not a pixel had
// changed, and whatever the guest was halfway through drawing. The display
// thread takes frames on its own schedule now; see DoomSystem::display_loop.
struct Snapshot {
	// Increments on every publish, so a reader can tell a new snapshot from
	// the one it already drew without comparing the contents.
	uint64_t seq = 0;
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
	HistoryEntry trace[13]{};

	// CSRs panel (dashboard) -- mirrors Registers::csr_history, but with
	// each entry's *live* value already read out too, so the panel shows
	// current contents, not just which addresses have been touched.
	struct CsrEntry {
		uint16_t addr = 0;
		uint64_t value = 0;
	};
	static constexpr int CSR_PANEL_SIZE = Registers::CSR_TOP;
	CsrEntry csrs[CSR_PANEL_SIZE]{};
	int csr_count = 0;
};
