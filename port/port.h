// The seam between DOOM and the machine it runs on.
//
// doomgeneric already reduces a platform to six DG_* functions. This port
// keeps that idea but splits it once more, so the *same* DG_* code runs on
// all three targets and only a small backend differs:
//
//   firmware/  the Pico 2 (RP2350, Hazard3 RISC-V cores, ILI9341, buttons)
//   doomv/     a DoomV guest, replaying what a Pico did and checking it
//   sim/       a PC build with the Pico's memory limits, for measuring
//
// Everything in port/ is shared. A backend implements the functions below
// and nothing else. Because the DG_* layer (port_link.c) is shared, the
// stream of values the engine sees -- every clock read, every key -- can be
// recorded on one target and replayed on another; that is what lets DoomV
// run in lock-step with real hardware.

#pragma once

#include <stdint.h>
#include <stddef.h>

#define PORT_SCREEN_W 320
#define PORT_SCREEN_H 200

// ---- implemented by each backend ----------------------------------------

// Bring up the display, input and link. Called once, before DOOM starts.
void backend_init(void);

// Milliseconds since boot. Only port_link.c calls this; everything in the
// engine goes through DG_GetTicksMs so the value can be logged/replayed.
uint32_t backend_ticks_ms(void);
void backend_sleep_ms(uint32_t ms);

// A free-running cycle counter for profiling (mcycle on RISC-V). Never seen
// by the engine, so it may differ freely between targets.
uint64_t backend_cycles(void);
uint32_t backend_cycles_per_us(void);

// Next raw input event: returns 1 and fills *pressed/*key (a DOOM key code
// from doomkeys.h), or 0 when there is nothing pending.
int backend_get_key(int *pressed, unsigned char *key);

// Show one finished 320x200 frame. `frame` is DOOM's own screen; `pal565`
// the palette as RGB565 and `pal_rgb` as 768 bytes of RGB.
//
// `tail` is a private copy of the frame's last `tail_rows` rows (0 to 200;
// NULL when 0), which DOOM will not touch until backend_present_wait()
// returns -- nor pal565, when tail_rows > 0. So a backend may return as soon
// as it has read the *other* rows from `frame`, and send the tail while DOOM
// draws the next frame (the Pico's second core does). All 200 rows copied:
// no waiting at all; none: wait for the whole frame.
void backend_present(const uint8_t *frame, const uint16_t *pal565, const uint8_t *pal_rgb,
                     const uint8_t *tail, int tail_rows);
// Block until the backend has finished reading the last frame it was given.
void backend_present_wait(void);

// The packed IWAD, mapped read-only (flash on the Pico).
const uint8_t *backend_wad(uint32_t *size);

// The memory DOOM's zone allocator may use. The Pico returns everything
// left over in SRAM; the sim returns exactly what a Pico would have.
uint8_t *backend_zone(int *size);

// Report a line of human-readable status (perf stats, errors).
void backend_log(const char *line);

// DOOM stopped (I_Error or quit). Show `msg` and never return.
void backend_fatal(const char *msg) __attribute__((noreturn));

// Lock-step (see docs/LOCKSTEP.md). A backend is in one of three modes:
//   LINK_NONE    clock and keys come straight from the backend.
//   LINK_RECORD  the same, and every value is also sent with link_write
//                (the Pico, when picolink.py is attached).
//   LINK_REPLAY  clock and keys come from replay_ticks/replay_key instead,
//                i.e. from a recording (DoomV). The replay side compares
//                hashes in link_check and stops the machine on a mismatch.
enum port_link_mode { LINK_NONE, LINK_RECORD, LINK_REPLAY };
enum port_link_mode backend_link_mode(void);
void backend_link_write(const void *data, size_t len);
uint32_t backend_replay_ticks(void);
int backend_replay_key(int *pressed, unsigned char *key);
// instret_tic / instret_render: instructions *retired* this frame in game
// logic and in rendering -- execution compared at retirement, so the Pico's
// pipeline (what it fetched, squashed or stalled on) cannot show up in it.
void backend_link_check(uint32_t frame, uint32_t frame_hash, uint32_t state_hash,
                        uint32_t instret_tic, uint32_t instret_render);

// Optional: called once a second with a status line for an on-screen HUD.
void backend_hud(const char *text);
