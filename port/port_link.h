// Lock-step between a Pico and DoomV. See docs/LOCKSTEP.md.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Wire format, Pico -> PC, little-endian. Every record starts with a tag
// byte. The engine's only inputs are the clock and the keys, so these
// records are a complete description of a run: replay them into the same
// code and it must do exactly the same thing.
enum {
	LINK_TAG_TICKS     = 0x01,  // u32 value DG_GetTicksMs returned
	LINK_TAG_TICKS_REP = 0x02,  // u8  n: the previous value returned n more times
	LINK_TAG_KEY       = 0x03,  // u8 pressed, u8 key: DG_GetKey returned 1
	LINK_TAG_NOKEY     = 0x04,  //   DG_GetKey returned 0 (no event)
	LINK_TAG_FRAME     = 0x05,  // u32 frame, u32 frame_hash, u32 state_hash,
	                            // u32 retired instructions in game logic, u32 in rendering
	LINK_TAG_HELLO     = 0x06,  // u32 magic 'P2DL', u32 version, u32 wad_size, u32 zone_size
	LINK_TAG_VIDEO     = 0x07,  // u32 frame, then 320*200 bytes, 8-bit indexed
	LINK_TAG_PALETTE   = 0x08,  // 768 bytes RGB
	LINK_TAG_TEXT      = 0x09,  // u8 len, len bytes: a log line
	LINK_TAG_ARGS      = 0x0A,  // u8 len, len bytes: DOOM's extra arguments, space-separated
	                            //   (always right after HELLO; the replay must start DOOM the same way)
	LINK_TAG_INPUT     = 0x0B,  // u8 source (0 buttons, 1 PC), u8 pressed, u8 key: a key event
	                            //   as it reached DOOM, for the PC to see. Informational: the KEY
	                            //   record that follows is what a replay uses.
};

#define LINK_MAGIC   0x4C443250u  // "P2DL"
#define LINK_VERSION 2

// The replay side never parses this: DoomV's link device does, and hands
// the guest one clock value or key per register read. A guest asking for
// a clock value when the recording says "key" is itself a divergence.

void port_link_start(uint32_t wad_size, uint32_t zone_size, const char *args);
void port_link_frame(const uint8_t *screen, size_t len);
uint32_t port_link_frames(void);
void port_text(const char *line);   // diagnostics: in the recording, or on the console

// Retired instructions spent inside DG_GetTicksMs/DG_GetKey so far. Those are
// platform code (USB on the Pico, a replay device on DoomV), so the counted
// engine windows subtract them (port_perf.c).
uint32_t port_link_excluded_instret(void);
void port_link_exclude_instret(uint32_t n);   // add platform code run elsewhere (port_libc.c)

// Hashes, also used by the sim for golden-run checks.
uint32_t port_hash_bytes(const void *data, size_t len, uint32_t h);
uint32_t port_hash_game_state(void);
