// doomgeneric's DG_* interface, shared by every target, plus lock-step.
//
// DOOM is deterministic: given the same WAD, the same clock readings and the
// same key events in the same order, the same code computes the same frames.
// Those two inputs reach the engine through exactly two calls,
// DG_GetTicksMs and DG_GetKey, and both are implemented here, once, for
// every target. So:
//
//   * a recording target (the Pico) logs every value they return, and once
//     per frame a hash of the frame it drew and of the game state;
//   * a replaying target (DoomV) returns the logged values instead of its
//     own, computes the same hashes, and stops at the first frame where they
//     differ.
//
// Hashes never include pointers -- the two machines put the zone at
// different addresses -- only the values the game computes.

#include <string.h>

#include "doomgeneric.h"
#include "doomstat.h"
#include "m_random.h"
#include "p_local.h"
#include "r_state.h"

#include "port.h"
#include "port_link.h"
#include "port_perf.h"
#include "port_instret.h"

extern int rndindex, prndindex;   // m_random.c: the menu and game RNG positions

static enum port_link_mode mode;
static uint32_t frames;
static uint32_t excluded_instret;

uint32_t port_link_excluded_instret(void) { return excluded_instret; }

// The last clock value sent, and how many times since it was repeated.
// TryRunTics spins on the clock while it waits for the next tic, so most
// readings repeat the previous one; a repeat costs one byte, not five.
static uint32_t last_ticks;
static uint32_t repeats;
static int have_last;

static void put(const void *p, size_t n) { backend_link_write(p, n); }
static void put8(uint8_t v) { put(&v, 1); }
static void put32(uint32_t v)
{
	uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
	put(b, 4);
}

static void flush_repeats(void)
{
	while (repeats) {
		uint32_t n = repeats > 255 ? 255 : repeats;
		put8(LINK_TAG_TICKS_REP);
		put8((uint8_t)n);
		repeats -= n;
	}
}

void port_link_start(uint32_t wad_size, uint32_t zone_size, const char *args)
{
	mode = backend_link_mode();
	if (mode != LINK_RECORD) return;
	put8(LINK_TAG_HELLO);
	put32(LINK_MAGIC);
	put32(LINK_VERSION);
	put32(wad_size);
	put32(zone_size);   // the replay must give DOOM exactly the same heap
	size_t n = strlen(args);
	if (n > 255) n = 255;
	put8(LINK_TAG_ARGS);
	put8((uint8_t)n);
	put(args, n);
}

static uint32_t ticks_body(void);
static int key_body(int *pressed, unsigned char *key);

// The two engine inputs. Their bodies are platform code, so the retired
// instructions they take are set aside (see port_link_excluded_instret).
uint32_t DG_GetTicksMs(void)
{
	uint32_t i0 = port_instret();
	uint32_t t = ticks_body();
	excluded_instret += port_instret() - i0;
	return t;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
	uint32_t i0 = port_instret();
	int got = key_body(pressed, key);
	excluded_instret += port_instret() - i0;
	return got;
}

static uint32_t ticks_body(void)
{
	if (mode == LINK_REPLAY) return backend_replay_ticks();

	uint32_t t = backend_ticks_ms();
	if (mode == LINK_RECORD) {
		if (have_last && t == last_ticks) {
			repeats++;
		} else {
			flush_repeats();
			put8(LINK_TAG_TICKS);
			put32(t);
			last_ticks = t;
			have_last = 1;
		}
	}
	return t;
}

static int key_body(int *pressed, unsigned char *key)
{
	if (mode == LINK_REPLAY) return backend_replay_key(pressed, key);

	int got = backend_get_key(pressed, key);
	if (mode == LINK_RECORD) {
		flush_repeats();
		if (got) {
			put8(LINK_TAG_KEY);
			put8((uint8_t)*pressed);
			put8(*key);
		} else {
			put8(LINK_TAG_NOKEY);
		}
	}
	return got;
}

void DG_SleepMs(uint32_t ms) { backend_sleep_ms(ms); }
void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SetWindowTitle(const char *title) { (void)title; }

// ---- hashing ------------------------------------------------------------

// FNV-1a over 32-bit words. Not cryptographic; it only has to make two
// different frames very unlikely to collide, and be cheap on a Hazard3
// (~4 cycles a word: 16,000 words a frame, ~0.4 ms at 150 MHz).
uint32_t port_hash_bytes(const void *data, size_t len, uint32_t h)
{
	const uint8_t *p = data;
	for (; len >= 4; len -= 4, p += 4) {
		uint32_t w = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		h = (h ^ w) * 16777619u;
	}
	for (; len; len--, p++) h = (h ^ *p) * 16777619u;
	return h;
}

static uint32_t mix(uint32_t h, int32_t v) { return (h ^ (uint32_t)v) * 16777619u; }

uint32_t port_hash_game_state(void)
{
	uint32_t h = 2166136261u;
	h = mix(h, gametic);
	h = mix(h, gamestate);
	h = mix(h, leveltime);
	h = mix(h, prndindex);
	h = mix(h, rndindex);
	if (gamestate != GS_LEVEL) return h;

	for (thinker_t *th = thinkercap.next; th && th != &thinkercap; th = th->next) {
		if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
		mobj_t *mo = (mobj_t *)th;
		h = mix(h, mo->type);
		h = mix(h, mo->x);
		h = mix(h, mo->y);
		h = mix(h, mo->z);
		h = mix(h, mo->momx);
		h = mix(h, mo->momy);
		h = mix(h, mo->momz);
		h = mix(h, (int32_t)mo->angle);
		h = mix(h, mo->health);
		h = mix(h, mo->tics);
		h = mix(h, (int32_t)(mo->state - states));
		h = mix(h, mo->flags);
	}
	for (int i = 0; i < numsectors; i++) {
		h = mix(h, sectors[i].floorheight);
		h = mix(h, sectors[i].ceilingheight);
		h = mix(h, sectors[i].lightlevel);
	}
	player_t *p = &players[consoleplayer];
	h = mix(h, p->health);
	h = mix(h, p->armorpoints);
	h = mix(h, p->readyweapon);
	h = mix(h, p->viewz);
	return h;
}

void port_link_frame(const uint8_t *screen, size_t len)
{
	frames++;
	if (mode == LINK_NONE) return;

	uint64_t t0 = backend_cycles();
	uint32_t fh = port_hash_bytes(screen, len, 2166136261u);
	uint32_t sh = port_hash_game_state();
	uint32_t it, ir;
	perf_take_instret(&it, &ir);
	perf_add_link(backend_cycles() - t0);

	if (mode == LINK_RECORD) {
		flush_repeats();
		put8(LINK_TAG_FRAME);
		put32(frames);
		put32(fh);
		put32(sh);
		put32(it);
		put32(ir);
	}
	backend_link_check(frames, fh, sh, it, ir);
}

uint32_t port_link_frames(void) { return frames; }
