#pragma once
#include "timer.hpp"
#include "imsic.hpp"
#include "aplic.hpp"
#include <cstdint>
#include <mutex>
#include <vector>

class Memory {
public:
	// MMIO/RAM/WAD addresses are still small values that fit comfortably in
	// 64 bits -- RV64 doesn't require actually using a 64-bit-wide address
	// space, just being able to represent one.
	static constexpr uint64_t MMIO_INPUT = 0x10000000;
	static constexpr uint64_t MMIO_TICK  = 0x10000004;
	static constexpr uint64_t MMIO_DEBUG = 0x10000008;
	static constexpr uint64_t MMIO_FB    = 0x10001000;

	static constexpr int FB_W = 320;
	static constexpr int FB_H = 200;
	static constexpr uint32_t FB_SIZE = FB_W * FB_H * 4; // 32bpp, matches doomgeneric's native output

	// RAM_BASE moved from the original 0x10041000 for Stage 3: OpenSBI's
	// `generic` platform build hardcodes its own load/entry address
	// (FW_TEXT_START, see tools/opensbi/build.sh) to 0x80000000 and
	// requires it 2MB-aligned -- 0x10041000 wasn't, and rather than
	// override+rebuild OpenSBI to match some other aligned address,
	// matching OpenSBI's own already-built default is strictly simpler
	// (real hardware/QEMU-virt convention too). RAM_SIZE grew from 16MB
	// since even just the DTB load offset OpenSBI expects
	// (FW_TEXT_START + 0x2200000) is ~34MB in, before the ~23MB kernel
	// Image or any of Linux's own runtime allocation.
	static constexpr uint64_t RAM_BASE  = 0x80000000;
	static constexpr uint64_t RAM_SIZE  = 256 * 1024 * 1024;
	static constexpr uint64_t WAD_BASE  = RAM_BASE + RAM_SIZE;
	static constexpr uint64_t WAD_SIZE  = 20 * 1024 * 1024;

	// Platform devices added for Stage 2 (timer + AIA interrupt
	// controller) -- addresses follow common real-hardware/QEMU-virt
	// convention (helps Stage 3's device tree resemble known-good
	// examples), and don't collide with anything above.
	static constexpr uint64_t CLINT_BASE = 0x02000000;
	static constexpr uint64_t CLINT_SIZE = 0x10000;
	static constexpr uint64_t APLIC_BASE = 0x0C000000;
	static constexpr uint64_t APLIC_SIZE = 0x4000;
	static constexpr uint64_t IMSIC_M_BASE = 0x24000000;
	static constexpr uint64_t IMSIC_S_BASE = 0x28000000;
	static constexpr uint64_t IMSIC_SIZE   = 0x1000;

	Memory();

	uint8_t  read8(uint64_t addr);
	uint16_t read16(uint64_t addr);
	uint32_t read32(uint64_t addr);
	uint64_t read64(uint64_t addr);
	void     write8(uint64_t addr, uint8_t val);
	void     write32(uint64_t addr, uint32_t val);
	void     write64(uint64_t addr, uint64_t val);

	bool load_elf(const char *path);

	// For anything that isn't an ELF -- the Linux kernel's `Image` (a raw
	// flat binary) and a compiled device tree blob. Reads the whole file
	// and copies it in starting at `addr` (physical), bounds-checked
	// against the RAM+WAD span the same way load_elf's segments are.
	bool load_blob(const char *path, uint64_t addr);

	// Copies wad_bytes into the WAD region at WAD_BASE.
	bool load_wad(const uint8_t *wad_bytes, size_t len);

	void push_key_event(bool pressed, uint8_t doom_keycode);

	// Instructions-per-ms calibrated to observed throughput: DoomSystem
	// bursts 20000 instructions per render at ~60fps (~16.7ms/frame), so
	// roughly 1200 instructions per "ms" tracks close to real 35Hz tic
	// pacing. A naive 1:1 mapping (1 instruction = 1ms) made simulated
	// time race ~1000x too fast, leaving I_GetTime()-driven game logic
	// stuck trying to process an enormous backlog of tics it thought had
	// already elapsed -- see PLAN.md's debugging notes.
	static constexpr uint32_t INSTR_PER_MS = 1200;
	void step_instructions(uint32_t count);

	const uint8_t *framebuffer() const { return fb.data(); }

	// Returns the number of FB writes since the last call, and resets
	// the counter. Used for the doom_fps dashboard metric.
	uint32_t take_fb_write_count();

	// Exposed for ext_zicsr.cpp's mip/mie/miselect-mireg CSR handling and
	// RiscvCore::check_and_take_interrupt -- CSR logic needs to read the
	// timer/IMSIC state that backs the computed MTIP/STIP/MEIP/SEIP bits.
	Timer &get_timer() { return timer; }
	Imsic &get_imsic_m() { return imsic_m; }
	Imsic &get_imsic_s() { return imsic_s; }

private:
	// RAM and WAD are contiguous (RAM_BASE..RAM_BASE+RAM_SIZE == WAD_BASE),
	// so one backing buffer covers both -- see the memory map in PLAN.md.
	std::vector<uint8_t> ram;
	std::vector<uint8_t> fb;

	// Pushed by the render thread (input polling lives there, tied to the
	// SDL window), popped by the CPU thread on MMIO_INPUT reads -- the one
	// piece of Memory state actually touched from both threads.
	std::mutex key_mutex;
	uint32_t key_queue[16];
	int key_queue_head;
	int key_queue_tail;

	uint32_t instr_count;   // raw executed-instruction count
	uint32_t tick_counter;  // instr_count / INSTR_PER_MS -- what MMIO_TICK exposes
	uint32_t ms_accum;      // instructions banked toward the next tick_counter++ (avoids a divide every instruction)
	uint32_t fb_write_count;

	// Declaration order matters here: aplic's constructor takes a
	// reference to imsic_s, so imsic_s must finish constructing first --
	// C++ initializes members in declaration order regardless of the
	// initializer-list order in memory.cpp.
	Timer timer;
	Imsic imsic_m;
	Imsic imsic_s;
	Aplic aplic;
};
