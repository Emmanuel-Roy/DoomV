// The DoomV backend for the shared port layer (port/port.h).
//
// With a recording attached (DoomV's -picolink=FILE), every clock reading and
// key event comes from the Pico's recording, the zone is exactly the size the
// Pico had, and at the end of every frame the frame hash, the game-state hash
// and the retired-instruction counts go to DoomV, which compares them with
// what the Pico reported and halts at the first difference.
//
// Without one, this is just DOOM on DoomV: its clock, keyboard and window.

#include <stdint.h>
#include <string.h>

#include "doomv_mmio.h"
#include "port.h"

void port_run(char **extra);

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static enum port_link_mode mode;

// ---- clock, keys, cycles ----------------------------------------------------

uint32_t backend_ticks_ms(void) { return REG(MMIO_TICK); }

void backend_sleep_ms(uint32_t ms)
{
	uint32_t start = REG(MMIO_TICK);
	while (REG(MMIO_TICK) - start < ms) {}
}

uint64_t backend_cycles(void)
{
	uint32_t lo, hi;
	__asm__ volatile("csrr %0, mcycle" : "=r"(lo));
	__asm__ volatile("csrr %0, mcycleh" : "=r"(hi));
	return ((uint64_t)hi << 32) | lo;
}

uint32_t backend_cycles_per_us(void) { return 1; }   // DoomV's timing is not the Pico's

int backend_get_key(int *pressed, unsigned char *key)
{
	uint32_t v = REG(MMIO_INPUT);
	if (!v) return 0;
	*pressed = (v >> 8) & 0xFF;
	*key = v & 0xFF;
	return 1;
}

// ---- display ------------------------------------------------------------------

void backend_present_wait(void) {}

void backend_present(const uint8_t *frame, const uint16_t *pal565, const uint8_t *pal_rgb,
                     const uint8_t *tail, int tail_rows)
{
	(void)tail; (void)tail_rows;   // presented at once from `frame`: nothing to wait for
	(void)pal565;
	static uint32_t pal32[256];
	for (int i = 0; i < 256; i++)
		pal32[i] = ((uint32_t)pal_rgb[3 * i] << 16) | ((uint32_t)pal_rgb[3 * i + 1] << 8) | pal_rgb[3 * i + 2];
	volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)MMIO_FB;
	for (int i = 0; i < PORT_SCREEN_W * PORT_SCREEN_H; i++) fb[i] = pal32[frame[i]];
}

void backend_hud(const char *text) { (void)text; }

// ---- WAD and memory -------------------------------------------------------------

const uint8_t *backend_wad(uint32_t *size)
{
	*size = REG(MMIO_WAD_SIZE);
	return (const uint8_t *)(uintptr_t)REG(MMIO_WAD_BASE);
}

uint8_t *backend_zone(int *size)
{
	// Replaying: exactly the Pico's zone size, so DOOM's allocator makes the
	// same decisions (which cache blocks it purges, whether the melt fits).
	static uint8_t zone[1024 * 1024] __attribute__((aligned(8)));
	uint32_t z = REG(MMIO_LINK_ZONE);
	*size = z ? (int)z : (int)sizeof zone;
	if (*size > (int)sizeof zone) backend_fatal("recorded zone is larger than the guest's buffer");
	return zone;
}

// ---- console ----------------------------------------------------------------------

static void puts_debug(const char *s)
{
	while (*s) *(volatile uint8_t *)(uintptr_t)MMIO_DEBUG = (uint8_t)*s++;
}

void backend_log(const char *line) { puts_debug(line); puts_debug("\n"); }

void backend_fatal(const char *msg)
{
	puts_debug("DOOM stopped: ");
	puts_debug(msg);
	puts_debug("\n");
	for (;;) {}
}

// ---- lock-step ------------------------------------------------------------------------

enum port_link_mode backend_link_mode(void) { return mode; }
void backend_link_write(const void *data, size_t len) { (void)data; (void)len; }
uint32_t backend_replay_ticks(void) { return REG(MMIO_LINK_TICKS); }

int backend_replay_key(int *pressed, unsigned char *key)
{
	uint32_t v = REG(MMIO_LINK_KEY);
	if (!v) return 0;
	*pressed = (v >> 8) & 0xFF;
	*key = v & 0xFF;
	return 1;
}

void backend_link_check(uint32_t frame, uint32_t frame_hash, uint32_t state_hash,
                        uint32_t instret_tic, uint32_t instret_render)
{
	if (mode != LINK_REPLAY) return;
	REG(MMIO_LINK_FRAME) = frame;
	REG(MMIO_LINK_FRAME) = frame_hash;
	REG(MMIO_LINK_FRAME) = state_hash;
	REG(MMIO_LINK_FRAME) = instret_tic;
	REG(MMIO_LINK_FRAME) = instret_render;   // DoomV compares now
}

void backend_init(void) {}

int main(void)
{
	mode = REG(MMIO_LINK_ZONE) ? LINK_REPLAY : LINK_NONE;
	backend_log(mode == LINK_REPLAY ? "pico2doom: replaying a Pico 2 recording in lock-step"
	                                : "pico2doom: running on DoomV (no recording attached)");

	// Start DOOM with the arguments the Pico used (e.g. -timedemo demo1).
	static char args[256];
	static char *argv[16];
	int n = 0, argc = 0;
	if (mode == LINK_REPLAY) {
		for (uint32_t c; n < (int)sizeof args - 1 && (c = REG(MMIO_LINK_ARGS)) != 0;) args[n++] = (char)c;
	}
	args[n] = 0;
	for (char *p = strtok(args, " "); p && argc < 15; p = strtok(NULL, " ")) argv[argc++] = p;
	argv[argc] = NULL;
	port_run(argv);
	return 0;
}
