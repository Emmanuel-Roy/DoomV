#pragma once

// Pico2Doom lock-step: DoomV replays what a real Raspberry Pi Pico 2 did.
//
// The Pico (Hazard3 RISC-V cores) records every value DOOM's two inputs
// returned -- each clock reading and each key event -- and, once per frame,
// a hash of the frame it drew, a hash of the game state, and how many
// instructions it *retired* in DOOM's game logic and in its renderer
// (minstret). tools/picolink.py in the Pico2Doom repository writes that
// stream to a file as it arrives.
//
// -picolink=FILE attaches it. The guest (Pico2Doom's doomv/ build: the same
// machine code as the firmware) reads its clock and keys from the registers
// below instead of the usual ones, and hands in its own hashes and retired
// counts each frame. The first frame where anything differs -- or the first
// time the guest asks for an input the Pico did not read at that point --
// halts the machine with crash.log written, so DoomV's own tools (Sail
// lock-step, the debugger) can take it from there.
//
// Compared at retirement on purpose: the Pico's pipeline has instructions in
// flight that may never complete, and only retired instructions are
// architectural. DoomV executes one instruction at a time, so its minstret
// is directly comparable.
//
// The file may still be growing (a live run): reads wait for more data.
//
// Co-running (Pico2Doom's `picolink.py corun`): the PC records the Pico into
// the file while DoomV follows it, and with PICOLINK_ACK set DoomV prints
// "picolink-ack N" to stderr after each frame that matched. The PC uses it to
// keep the Pico at most a few frames ahead, and to stop the Pico at the frame
// where DoomV halted.

#include <cstdint>
#include <cstdio>
#include <string>

class PicoLink {
public:
	static constexpr uint64_t MMIO_ZONE  = 0x1000001C;  // R: recorded zone size
	static constexpr uint64_t MMIO_TICKS = 0x10000020;  // R: next clock value
	static constexpr uint64_t MMIO_KEY   = 0x10000024;  // R: next key: 0, or 0x10000|pressed<<8|key
	static constexpr uint64_t MMIO_FRAME = 0x10000028;  // W x5: frame, fhash, shash, instret logic, instret render
	static constexpr uint64_t MMIO_ARGS  = 0x1000002C;  // R: next byte of the Pico's DOOM arguments, 0 at the end

	static bool covers(uint64_t addr) { return addr >= MMIO_ZONE && addr < MMIO_ARGS + 4; }

	bool open(const std::string &path);
	uint32_t read32(uint64_t addr);
	void write32(uint64_t addr, uint32_t val);

	// Set at the first difference; DoomSystem halts the machine on it.
	bool diverged() const { return diverged_; }
	const std::string &report() const { return report_; }
	bool take_halt_request() { bool r = diverged_ && !halt_taken_; if (r) halt_taken_ = true; return r; }

private:
	uint8_t u8();
	uint32_t u32();
	void skip(size_t n);
	uint8_t next_input_tag();          // the next clock/key/frame record, skipping video and text
	void diverge(const std::string &why);
	void read_hello();

	FILE *f_ = nullptr;
	std::string path_;
	bool hello_ = false;
	uint32_t zone_ = 0;
	std::string args_;
	size_t args_pos_ = 0;
	bool retired_counts_ = true;    // false for a recording made without minstret (the PC sim)
	uint32_t last_ticks_ = 0;
	uint32_t repeats_ = 0;             // TICKS_REP: the last value, this many more times
	uint32_t frame_words_[5] = {};
	int frame_word_n_ = 0;
	uint64_t frames_ok_ = 0;
	uint64_t instret_total_ = 0;
	uint64_t instret_mismatches_ = 0;
	bool diverged_ = false, halt_taken_ = false;
	std::string report_;
};
