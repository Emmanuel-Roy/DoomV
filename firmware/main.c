// Pico2Doom: the RP2350 backend for the shared port layer (port/port.h).
//
// Core 0 runs DOOM. Core 1 runs USB (usb_link.c). Boot modes:
//
//   normal      play.
//   benchmark   hold FIRE while powering on (or `picolink.py bench`): runs
//               DOOM's own -timedemo on DEMO1 and shows the result.
//   link        `picolink.py` reboots the board into this mode: the whole
//               run is recorded over USB so DoomV can replay it in
//               lock-step (docs/LOCKSTEP.md).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"

#include "board_config.h"
#include "buttons.h"
#include "lcd_ili9341.h"
#include "usb_link.h"

#include "port.h"
#include "port_link.h"
#include "port_perf.h"

void port_run(char **extra);

extern const uint8_t doom_wad[], doom_wad_end[];

// Bytes kept back from the zone for DOOM's own malloc calls (the WAD
// strings; the WAD directory stays in flash, patches/05). The sim measured
// a peak of 113 bytes; this leaves room for newlib's own stdio buffers.
#ifndef DOOM_MALLOC_RESERVE
#define DOOM_MALLOC_RESERVE (8 * 1024)
#endif

// Watchdog scratch registers survive a reboot: picolink.py's commands set
// the next boot's mode there.
#define MODE_MAGIC 0xD0D0B000u
enum { MODE_PLAY, MODE_BENCH, MODE_LINK };

static enum port_link_mode link_mode = LINK_NONE;
static int video_every;          // stream every Nth frame to the PC (0: off)
static uint32_t frame_count;
static char hud_line[192];
static volatile int hud_dirty;

// ---- time -----------------------------------------------------------------

uint32_t backend_ticks_ms(void) { return to_ms_since_boot(get_absolute_time()); }
// In link mode core 0 must take no interrupts: a handler's instructions
// would retire inside a counted window and the retired-instruction counts
// compared with DoomV would differ. sleep_ms() sleeps on a timer interrupt,
// so link mode spins instead. (USB already lives on core 1.)
void backend_sleep_ms(uint32_t ms)
{
	if (link_mode == LINK_NONE) { sleep_ms(ms); return; }
	uint64_t until = time_us_64() + 1000ull * ms;
	while (time_us_64() < until) tight_loop_contents();
}

uint64_t backend_cycles(void)
{
	uint32_t hi, lo, hi2;
	do {
		__asm__ volatile("csrr %0, mcycleh" : "=r"(hi));
		__asm__ volatile("csrr %0, mcycle" : "=r"(lo));
		__asm__ volatile("csrr %0, mcycleh" : "=r"(hi2));
	} while (hi != hi2);
	return ((uint64_t)hi << 32) | lo;
}

static uint32_t cycles_per_us = 150;
uint32_t backend_cycles_per_us(void) { return cycles_per_us; }

// ---- stack high-water mark ------------------------------------------------

// Core 0's stack is SCRATCH_X + SCRATCH_Y (8 KB). Paint what is unused at
// boot; the deepest unpainted word later is the most DOOM ever used.
#define STACK_LO 0x20080000u
#define STACK_HI 0x20082000u
#define STACK_PAINT 0x5AC4ED00u

static void paint_stack(void)
{
	uint32_t sp;
	__asm__ volatile("mv %0, sp" : "=r"(sp));
	for (uint32_t *p = (uint32_t *)STACK_LO; (uint32_t)p < sp - 64; p++) *p = STACK_PAINT;
}

static unsigned stack_used(void)
{
	uint32_t *p = (uint32_t *)STACK_LO;
	while ((uint32_t)p < STACK_HI && *p == STACK_PAINT) p++;
	return STACK_HI - (uint32_t)p;
}

// ---- console --------------------------------------------------------------

void backend_log(const char *line)
{
	usb_console_write(line, strlen(line));
	usb_console_write("\r\n", 2);
}

static char banner[96];

void backend_hud(const char *text)
{
	// Repeat the clock banner now and then: the one at boot goes out before
	// a terminal has had a chance to open the console port.
	static unsigned calls;
	if (calls++ % 10 == 0) backend_log(banner);
	snprintf(hud_line, sizeof hud_line, "%s | stack %u B", text, stack_used());
	hud_dirty = 1;
}

// ---- display, input, WAD, memory -------------------------------------------

int backend_get_key(int *pressed, unsigned char *key) { return buttons_get_key(pressed, key); }

const uint8_t *backend_wad(uint32_t *size)
{
	*size = (uint32_t)(doom_wad_end - doom_wad);
	return doom_wad;
}

extern char __HeapLimit;
uint8_t *backend_zone(int *size)
{
	char *brk = sbrk(0);
	int avail = (int)(&__HeapLimit - brk);
	*size = (avail - DOOM_MALLOC_RESERVE) & ~7;
	uint8_t *z = malloc(*size);
	if (!z) backend_fatal("no RAM left for DOOM's zone");
	return z;
}

static void poll_host_commands(void)
{
	int c;
	while ((c = usb_link_getc()) >= 0) {
		switch (c) {
		case 'V': {                     // 'V' n: video every n frames
			int n = -1;
			for (int tries = 0; n < 0 && tries < 100000; tries++) n = usb_link_getc();
			video_every = n < 0 ? 0 : n;
			break;
		}
		case 'R':                       // reboot into link (lock-step) mode
			watchdog_hw->scratch[0] = MODE_MAGIC | MODE_LINK;
			watchdog_reboot(0, 0, 10);
			for (;;) {}
		case 'U':                       // reboot into BOOTSEL, ready for a new UF2
			reset_usb_boot(0, 0);
			for (;;) {}
		case 'B':                       // reboot into the benchmark
			watchdog_hw->scratch[0] = MODE_MAGIC | MODE_BENCH;
			watchdog_reboot(0, 0, 10);
			for (;;) {}
		}
	}
}

static void stream_video(const uint8_t *frame, const uint8_t *pal_rgb)
{
	static uint32_t last_pal_hash;
	uint32_t ph = port_hash_bytes(pal_rgb, 768, 2166136261u);
	if (ph != last_pal_hash) {
		uint8_t tag = LINK_TAG_PALETTE;
		usb_link_write(&tag, 1);
		usb_link_write(pal_rgb, 768);
		last_pal_hash = ph;
	}
	uint8_t hdr[5] = { LINK_TAG_VIDEO, (uint8_t)frame_count, (uint8_t)(frame_count >> 8),
	                   (uint8_t)(frame_count >> 16), (uint8_t)(frame_count >> 24) };
	usb_link_write(hdr, 5);
	usb_link_write(frame, PORT_SCREEN_W * PORT_SCREEN_H);
}

void backend_present(const uint8_t *frame, const uint16_t *pal565, const uint8_t *pal_rgb)
{
	frame_count++;
	lcd_present_frame(frame, pal565);
	if (hud_dirty) {
		hud_dirty = 0;
		// "fps 21.4 | frame ... " -> two HUD lines: timings, then memory.
		char *zone = strstr(hud_line, "| zone");
		if (zone) { zone[-1] = 0; lcd_hud_text(hud_line, zone + 2); }
		else lcd_hud_text(hud_line, "");
	}
	poll_host_commands();
	if (video_every && usb_link_connected() && frame_count % video_every == 0)
		stream_video(frame, pal_rgb);
}

// ---- lock-step --------------------------------------------------------------

enum port_link_mode backend_link_mode(void) { return link_mode; }
void backend_link_write(const void *data, size_t len) { usb_link_write(data, len); }
uint32_t backend_replay_ticks(void) { return 0; }
int backend_replay_key(int *p, unsigned char *k) { (void)p; (void)k; return 0; }
void backend_link_check(uint32_t f, uint32_t a, uint32_t b, uint32_t it, uint32_t ir)
{
	(void)f; (void)a; (void)b; (void)it; (void)ir;   // the Pico records; DoomV compares
}

// ---- stopping -----------------------------------------------------------------

void backend_fatal(const char *msg)
{
	char summary[160];
	const perf_window_t *t = perf_totals();
	perf_format(t, summary, sizeof summary);

	char text[512];
	snprintf(text, sizeof text, "%s\n\nWhole run: %s\nstack %u B\n\nPress any button to restart.",
	         msg, summary, stack_used());
	backend_log(msg);
	backend_log(summary);
	if (link_mode == LINK_RECORD) {
		uint8_t hdr[2] = { LINK_TAG_TEXT, (uint8_t)(strlen(msg) > 255 ? 255 : strlen(msg)) };
		usb_link_write(hdr, 2);
		usb_link_write(msg, hdr[1]);
	}
	lcd_message(strncmp(msg, "timed ", 6) == 0 ? "Benchmark finished" : "DOOM stopped", text);
	usb_flush(500);

	// Release, then press -- still listening to the PC, so picolink.py can
	// reboot or reflash a board left on this screen (or one with no buttons).
	while (buttons_raw()) { poll_host_commands(); sleep_ms(10); }
	while (!buttons_raw()) { poll_host_commands(); sleep_ms(10); }
	watchdog_hw->scratch[0] = 0;
	watchdog_reboot(0, 0, 10);
	for (;;) {}
}

// ---- boot ---------------------------------------------------------------------

void backend_init(void) {}

int main(void)
{
	paint_stack();

	if (DOOM_SYS_MHZ > 150) {
		vreg_set_voltage(VREG_VOLTAGE_1_20);   // headroom for an overclock
		sleep_ms(2);
	}
	set_sys_clock_khz(DOOM_SYS_MHZ * 1000, true);
	// Run the peripheral clock (and so the LCD's SPI) from the system clock.
	// Left alone it stays on the 48 MHz USB PLL, which caps SPI at 12 MHz:
	// measured on hardware, 95 ms to send one frame instead of 27 ms.
	clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
	                clock_get_hz(clk_sys), clock_get_hz(clk_sys));
	cycles_per_us = clock_get_hz(clk_sys) / 1000000;
	__asm__ volatile("csrw mcountinhibit, zero");   // make sure mcycle counts

	int mode = MODE_PLAY;
	if ((watchdog_hw->scratch[0] & 0xFFFFFF00u) == MODE_MAGIC) mode = watchdog_hw->scratch[0] & 0xFF;
	watchdog_hw->scratch[0] = 0;

	usb_start();
	buttons_init();
	if (buttons_raw() & (1u << BTN_FIRE)) mode = MODE_BENCH;

	uint32_t spi_hz = DOOM_LCD_SPI_HZ ? DOOM_LCD_SPI_HZ : clock_get_hz(clk_peri) / 4;
	spi_hz = lcd_init(spi_hz);

	char *line = banner;
	snprintf(banner, sizeof banner, "Pico2Doom: RP2350 Hazard3 @ %lu MHz, LCD SPI %lu.%lu MHz",
	         (unsigned long)(clock_get_hz(clk_sys) / 1000000),
	         (unsigned long)(spi_hz / 1000000), (unsigned long)(spi_hz / 100000 % 10));
	(void)sizeof line;

	char *extra[4] = { NULL };
	if (mode == MODE_BENCH) {
		extra[0] = "-timedemo";
		extra[1] = "demo1";
		lcd_message("Pico2Doom", "Benchmark: timedemo DEMO1...");
	}
	if (mode == MODE_LINK) {
		// picolink.py rebooted us and is reopening the port: wait for it, so
		// the recording starts at DOOM's very first clock read.
		lcd_message("Pico2Doom", "Link mode: waiting for picolink.py...");
		absolute_time_t until = make_timeout_time_ms(10000);
		while (!usb_link_connected() && !time_reached(until)) sleep_ms(10);
		if (usb_link_connected()) link_mode = LINK_RECORD;
	}
	sleep_ms(mode == MODE_PLAY ? 300 : 0);   // let a console attach and see the banner
	backend_log(line);
	if (link_mode == LINK_RECORD) backend_log("link: recording for DoomV lock-step");

	port_run(extra);
}
