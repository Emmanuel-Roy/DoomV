// Pico2Doom lock-step device. See pico_link.hpp.

#include "pico_link.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
// Pico2Doom's port/port_link.h, version 2.
enum : uint8_t {
	TAG_TICKS = 0x01, TAG_TICKS_REP = 0x02, TAG_KEY = 0x03, TAG_NOKEY = 0x04,
	TAG_FRAME = 0x05, TAG_HELLO = 0x06, TAG_VIDEO = 0x07, TAG_PALETTE = 0x08, TAG_TEXT = 0x09,
	TAG_ARGS = 0x0A, TAG_INPUT = 0x0B,
};
constexpr uint32_t LINK_MAGIC = 0x4C443250u;  // "P2DL"
constexpr uint32_t LINK_VERSION = 2;

const char *tag_name(uint8_t t)
{
	switch (t) {
	case TAG_TICKS: case TAG_TICKS_REP: return "a clock reading";
	case TAG_KEY: case TAG_NOKEY: return "a key read";
	case TAG_FRAME: return "the end of a frame";
	default: return "an unknown record";
	}
}
}

bool PicoLink::open(const std::string &path)
{
	path_ = path;
	f_ = std::fopen(path.c_str(), "rb");
	if (!f_) {
		std::cerr << "-picolink=" << path << ": cannot open\n";
		return false;
	}
	return true;
}

// The recording may still be arriving from the Pico: wait for each byte.
// Live (picolink.py corun) the next record is usually a millisecond or two
// away, and a sleep on Windows can last a 15.6 ms timer tick, which paced the
// Pico at 10 fps: yield first, and sleep only once the stream has gone quiet.
uint8_t PicoLink::u8()
{
	for (unsigned idle = 0;; idle++) {
		int c = std::fgetc(f_);
		if (c != EOF) return (uint8_t)c;
		std::clearerr(f_);
		if (idle < 20000) std::this_thread::yield();
		else std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

uint32_t PicoLink::u32()
{
	uint32_t v = u8();
	v |= (uint32_t)u8() << 8;
	v |= (uint32_t)u8() << 16;
	v |= (uint32_t)u8() << 24;
	return v;
}

void PicoLink::skip(size_t n)
{
	while (n--) u8();
}

void PicoLink::read_hello()
{
	if (hello_) return;
	uint8_t tag = u8();
	if (tag != TAG_HELLO) { diverge("the recording does not start with a HELLO record"); return; }
	uint32_t magic = u32(), version = u32(), wad = u32();
	zone_ = u32();
	hello_ = true;
	if (magic != LINK_MAGIC || version != LINK_VERSION) {
		std::ostringstream m;
		m << "recording format " << std::hex << magic << " v" << std::dec << version
		  << ", expected v" << LINK_VERSION;
		diverge(m.str());
		return;
	}
	// DOOM's extra arguments (e.g. "-timedemo demo1"): the guest starts with them.
	if (u8() != TAG_ARGS) { diverge("the recording has no ARGS record after HELLO"); return; }
	args_.assign(u8(), '\0');
	for (char &c : args_) c = (char)u8();
	std::cerr << "picolink: replaying " << path_ << " (WAD " << wad << " bytes, zone "
	          << zone_ / 1024 << " KB, DOOM arguments \"" << args_ << "\")\n";
}

uint8_t PicoLink::next_input_tag()
{
	read_hello();
	for (;;) {
		uint8_t tag = u8();
		switch (tag) {
		case TAG_VIDEO: skip(4 + 320 * 200); continue;   // picolink.py shows it
		case TAG_PALETTE: skip(768); continue;
		case TAG_INPUT: skip(3); continue;              // a key's source, for the PC to show; KEY records drive the replay
		case TAG_TEXT: {
			std::string s(u8(), '\0');
			for (char &c : s) c = (char)u8();
			std::cerr << "picolink: Pico says: " << s << "\n";
			continue;
		}
		default: return tag;
		}
	}
}

void PicoLink::diverge(const std::string &why)
{
	if (diverged_) return;
	diverged_ = true;
	std::ostringstream m;
	m << "picolink: DIVERGED after " << frames_ok_ << " frames in step ("
	  << instret_total_ << " retired instructions of DOOM compared): " << why;
	report_ = m.str();
	std::cerr << report_ << "\n";
}

uint32_t PicoLink::read32(uint64_t addr)
{
	if (diverged_) return 0;
	if (addr == MMIO_ZONE) {
		read_hello();
		return zone_;
	}
	if (addr == MMIO_ARGS) {
		read_hello();
		return args_pos_ < args_.size() ? (uint8_t)args_[args_pos_++] : 0;
	}
	if (addr == MMIO_TICKS) {
		if (repeats_) { repeats_--; return last_ticks_; }
		uint8_t tag = next_input_tag();
		if (tag == TAG_TICKS) { last_ticks_ = u32(); return last_ticks_; }
		if (tag == TAG_TICKS_REP) { repeats_ = u8() - 1u; return last_ticks_; }
		diverge(std::string("DOOM read the clock, but the Pico's next input was ") + tag_name(tag));
		return 0;
	}
	if (addr == MMIO_KEY) {
		if (repeats_) { diverge("DOOM read a key, but the Pico read the clock again here"); return 0; }
		uint8_t tag = next_input_tag();
		if (tag == TAG_NOKEY) return 0;
		if (tag == TAG_KEY) {
			uint32_t pressed = u8(), key = u8();
			return 0x10000u | (pressed << 8) | key;
		}
		diverge(std::string("DOOM read a key, but the Pico's next input was ") + tag_name(tag));
		return 0;
	}
	return 0;
}

void PicoLink::write32(uint64_t addr, uint32_t val)
{
	if (diverged_ || addr != MMIO_FRAME) return;
	frame_words_[frame_word_n_++] = val;
	if (frame_word_n_ < 5) return;
	frame_word_n_ = 0;

	if (repeats_) { diverge("DOOM finished a frame, but the Pico read the clock again first"); return; }
	uint8_t tag = next_input_tag();
	if (tag != TAG_FRAME) {
		diverge(std::string("DOOM finished a frame, but the Pico's next input was ") + tag_name(tag));
		return;
	}
	uint32_t pico[5];
	for (uint32_t &w : pico) w = u32();

	static const char *const what[5] = {
		"frame number", "frame hash (the picture)", "game-state hash",
		"instructions retired in game logic", "instructions retired in rendering",
	};
	// A recorder without minstret (the PC sim) sends zero counts: compare the
	// hashes only, and say so once.
	if (frames_ok_ == 0 && pico[3] == 0 && pico[4] == 0 && retired_counts_) {
		retired_counts_ = false;
		std::cerr << "picolink: this recording has no retired-instruction counts (not from a Pico); "
		             "comparing hashes only\n";
	}
	// PICOLINK_HASHES_ONLY=1: a diagnostic mode -- report retired-instruction
	// differences but halt only when the picture or the game state differs,
	// to see whether the two machines stay in step architecturally.
	static const bool hashes_only = std::getenv("PICOLINK_HASHES_ONLY") != nullptr;
	if (hashes_only && retired_counts_ && (pico[3] != frame_words_[3] || pico[4] != frame_words_[4])) {
		if (++instret_mismatches_ <= 20)
			std::cerr << "picolink: frame " << pico[0] << " retired counts differ (state and picture match): logic "
			          << (int64_t)frame_words_[3] - (int64_t)pico[3] << ", render "
			          << (int64_t)frame_words_[4] - (int64_t)pico[4] << " (DoomV - Pico)\n";
	}
	std::ostringstream diff;
	for (int i = 0; i < ((retired_counts_ && !hashes_only) ? 5 : 3); i++) {
		if (pico[i] == frame_words_[i]) continue;
		diff << "\n  " << what[i] << ": Pico " << pico[i] << ", DoomV " << frame_words_[i];
		if (i >= 3) diff << " (" << (int64_t)frame_words_[i] - (int64_t)pico[i] << ")";
	}
	if (!diff.str().empty()) {
		diverge("frame " + std::to_string(pico[0]) + " differs:" + diff.str());
		return;
	}
	frames_ok_++;
	instret_total_ += (uint64_t)pico[3] + pico[4];
	// PICOLINK_ACK=1 (picolink.py corun): say each frame that matched, so the
	// PC can let the Pico run on -- it keeps the Pico a few frames ahead of
	// DoomV at most, and stops it where DoomV halts.
	static const bool ack = std::getenv("PICOLINK_ACK") != nullptr;
	if (ack) {
		// One write, so the guest's console output (stdout, often the same
		// pipe) cannot land in the middle of it.
		char line[40];
		const int n = std::snprintf(line, sizeof line, "picolink-ack %llu\n", (unsigned long long)frames_ok_);
		std::fwrite(line, 1, (size_t)n, stderr);
		std::fflush(stderr);
	}
	if (frames_ok_ % 175 == 0)
		std::cerr << "picolink: " << frames_ok_ << " frames in step, " << instret_total_
		          << " retired instructions of DOOM matched\n";
}
