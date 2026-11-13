// Core 1: the I/O core.
//
// Core 0 runs DOOM and nothing else -- no interrupts, no waiting on devices
// (which also keeps its retired-instruction counts comparable with DoomV's,
// docs/LOCKSTEP.md). Core 1 loops over everything else:
//
//   * USB: TinyUSB's task, the console and link rings (usb_link.c), and
//     commands from the PC;
//   * buttons: sampled, debounced, turned into DOOM key events;
//   * the display: the frame core 0 handed over, converted and sent a strip
//     at a time, then the performance read-out under it.
//
// Key events from the buttons *and* from the PC (picolink.py's window) go
// into one input buffer, which core 0's DG_GetKey drains. Since every key
// DOOM reads is recorded for lock-step, DoomV sees PC and button input alike.
//
// Hand-overs are single-producer/single-consumer, so a memory fence on each
// side is all the synchronisation needed.

#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"

#include "buttons.h"
#include "core1_io.h"
#include "lcd_ili9341.h"
#include "usb_link.h"

// ---- the input buffer: core 1 writes, core 0 reads ---------------------------

#define INPUT_SIZE 64
static struct { uint8_t source, pressed, key; } input[INPUT_SIZE];
static volatile uint32_t in_head, in_tail;

static void input_push(int source, int pressed, unsigned char key)
{
	if (in_head - in_tail >= INPUT_SIZE) return;     // full: DOOM drains it every tic
	uint32_t i = in_head % INPUT_SIZE;
	input[i].source = (uint8_t)source;
	input[i].pressed = (uint8_t)pressed;
	input[i].key = key;
	__mem_fence_release();
	in_head++;
}

static void button_event(int pressed, unsigned char key) { input_push(IO_FROM_BUTTONS, pressed, key); }

int io_get_key(int *pressed, unsigned char *key, int *source)
{
	if (in_head == in_tail) return 0;
	__mem_fence_acquire();
	uint32_t i = in_tail % INPUT_SIZE;
	*pressed = input[i].pressed;
	*key = input[i].key;
	*source = input[i].source;
	__mem_fence_release();
	in_tail++;
	return 1;
}

// ---- commands from the PC (link port) --------------------------------------
//
//   'K' pressed key   a key event, as from the buttons (picolink.py play)
//   'V' n             stream every nth frame over USB (0: stop)
//   'R'               reboot into link mode (lock-step recording)
//   'W' e m           the same, starting on map EeMm (-warp e m)
//   'B'               reboot into the benchmark
//   'U'               reboot into BOOTSEL, for a new UF2
//   'G' u32           co-run pacing: DOOM may finish frames up to this one,
//                     then waits for more (picolink.py corun; main.c)
//   'H' u32           co-run: DoomV diverged at this frame -- stop here

#define MODE_MAGIC 0xD0D0B000u
static volatile int video_every;
static volatile uint32_t link_allowed, link_halt_frame;
static volatile int link_paced, link_halted;

static void reboot_into(uint32_t mode)
{
	watchdog_hw->scratch[0] = MODE_MAGIC | mode;
	watchdog_reboot(0, 0, 10);
	for (;;) {}
}

static void host_byte(uint8_t c)
{
	static uint8_t cmd, args[4];
	static int nargs, need;
	if (need) {
		args[nargs++] = c;
		if (nargs < need) return;
		need = 0;
		if (cmd == 'K') input_push(IO_FROM_PC, args[0], args[1]);
		else if (cmd == 'W') {                  // link mode, starting on map E<a>M<b>
			watchdog_hw->scratch[1] = 0x57A00000u | ((uint32_t)args[0] << 8) | args[1];
			reboot_into(2);
		}
		else if (cmd == 'V') video_every = args[0];
		else if (cmd == 'G' || cmd == 'H') {
			uint32_t v = args[0] | (uint32_t)args[1] << 8 | (uint32_t)args[2] << 16 | (uint32_t)args[3] << 24;
			if (cmd == 'G') { link_allowed = v; __mem_fence_release(); link_paced = 1; }
			else { link_halt_frame = v; __mem_fence_release(); link_halted = 1; }
		}
		return;
	}
	switch (c) {
	case 'K': cmd = c; nargs = 0; need = 2; break;
	case 'W': cmd = c; nargs = 0; need = 2; break;
	case 'V': cmd = c; nargs = 0; need = 1; break;
	case 'G': case 'H': cmd = c; nargs = 0; need = 4; break;
	case 'R': reboot_into(2); break;   // main.c: MODE_LINK
	case 'B': reboot_into(1); break;   // main.c: MODE_BENCH
	case 'U': reset_usb_boot(0, 0); for (;;) {}
	}
}

int io_video_every(void) { return video_every; }

int io_link_wait(uint32_t frame, uint32_t *halt_frame)
{
	while (link_paced && frame >= link_allowed && !link_halted) tight_loop_contents();
	__mem_fence_acquire();
	if (link_halted) { *halt_frame = link_halt_frame; return 1; }
	return 0;
}

// ---- display jobs: core 0 posts one, core 1 does it ----------------------------

enum { JOB_NONE, JOB_FRAME, JOB_MESSAGE };
static volatile int job = JOB_NONE;
static const uint8_t *job_top, *job_tail;
static int job_tail_rows;
static const uint16_t *job_pal;
static volatile int job_rows_read;    // of the posted frame; core 0 waits on it
static const char *job_title, *job_msg;
static volatile uint32_t lcd_frame_us;

static char hud_next[192];
static volatile int hud_ready;

void io_present_wait(void)
{
	while (job != JOB_NONE) tight_loop_contents();
	__mem_fence_acquire();
}

void io_present(const uint8_t *top, const uint8_t *tail, int tail_rows, const uint16_t *pal565)
{
#if DOOM_LCD_TEAR
	// Never wait for the display (the default): a frame goes to the LCD when
	// core 1 is idle and is sent while DOOM draws the next one into the same
	// screen, so the LCD can show a torn frame. Without RAM for a second
	// 64 KB screen it is that or wait ~20 ms a frame; this keeps 30 fps.
	// (The lock-step hashes are taken before present: unaffected.)
	if (job != JOB_NONE) return;
	job_top = top; job_tail = tail; job_tail_rows = tail_rows; job_pal = pal565; job_rows_read = 0;
	__mem_fence_release();
	job = JOB_FRAME;
	return;
#endif
	io_present_wait();
	job_top = top;
	job_tail = tail;
	job_tail_rows = tail_rows;
	job_pal = pal565;
	job_rows_read = 0;
	__mem_fence_release();
	job = JOB_FRAME;
	// DOOM's own screen must be finished with before DOOM draws the next
	// frame into it; the copied tail (port_video.c's back buffer) is core 1's
	// until the next io_present_wait.
	const int split = 200 - tail_rows;
	while (job_rows_read < split) tight_loop_contents();
	__mem_fence_acquire();
}

void io_hud(const char *text)
{
	if (hud_ready) return;              // core 1 has not drawn the last one yet
	strncpy(hud_next, text, sizeof hud_next - 1);
	__mem_fence_release();
	hud_ready = 1;
}

void io_message(const char *title, const char *msg)
{
	io_present_wait();
	job_title = title;
	job_msg = msg;
	__mem_fence_release();
	job = JOB_MESSAGE;
	io_present_wait();
}

uint32_t io_lcd_frame_us(void) { return lcd_frame_us; }

static void draw_hud(void)
{
	static char line[192];
	__mem_fence_acquire();
	memcpy(line, hud_next, sizeof line);
	__mem_fence_release();
	hud_ready = 0;
	// "fps ... | lcd ... | zone ..." -> timings on top, memory below.
	char *zone = strstr(line, "| zone");
	if (zone) { zone[-1] = 0; lcd_hud_text(line, zone + 2); }
	else lcd_hud_text(line, "");
}

// ---- video for the PC: a half-size copy of the frames ----------------------------
//
// Core 1 reads every row of a frame for the display; on every video_every-th
// frame it also sends the even rows at half width on the video port (USB
// interface 4): a frame record (0x5A, u32 frame, the palette as 256 RGB565
// words), then a record per row (0xA5, row 0-99, 160 pixels). A record the
// USB FIFO cannot take at once is dropped -- the PC keeps that row's last
// picture -- so this never waits, and core 0 has no part in it. (Video used
// to go out from core 0 on the link port: 64 KB a frame at USB full speed
// held DOOM up for ~60 ms each time.)

#define VID_W 160
#define VID_H 100
static uint32_t video_frames;
static int video_row = -1;                     // next row to send; -1: not this frame
static const uint8_t *vid_top, *vid_tail;
static int vid_split;

static void video_begin(const uint8_t *top, const uint8_t *tail, int tail_rows, const uint16_t *pal)
{
	video_row = -1;
	int every = video_every;
	if (!every || !usb_video_open()) return;
	if (++video_frames % (uint32_t)every) return;
	uint8_t hdr[5] = { 0x5A, (uint8_t)video_frames, (uint8_t)(video_frames >> 8),
	                   (uint8_t)(video_frames >> 16), (uint8_t)(video_frames >> 24) };
	if (usb_video_room() < sizeof hdr + 512) return;   // still busy with the last one
	usb_video_put(hdr, sizeof hdr);
	usb_video_put(pal, 512);
	vid_top = top;
	vid_tail = tail;
	vid_split = 200 - tail_rows;
	video_row = 0;
}

// The rows whose source rows the display has read so far: core 0 may reuse
// them once job_rows_read says so, so they are sent before it does.
static void video_rows(int rows_read)
{
	static uint8_t rec[2 + VID_W];
	while (video_row >= 0 && video_row < VID_H && 2 * video_row < rows_read) {
		int y = 2 * video_row;
		const uint8_t *src = y < vid_split ? vid_top + y * 320 : vid_tail + (y - vid_split) * 320;
		if (usb_video_room() >= sizeof rec) {
			rec[0] = 0xA5;
			rec[1] = (uint8_t)video_row;
			for (int x = 0; x < VID_W; x++) rec[2 + x] = src[2 * x];
			usb_video_put(rec, sizeof rec);
		}
		video_row++;
	}
}

// ---- the loop -------------------------------------------------------------------

static uint32_t core1_stack[512];   // 2 KB in main RAM; core 0's stack owns SCRATCH_X/Y

static void core1_main(void)
{
	usb_init();
	bool sending = false;
	uint64_t t0 = 0;
	for (;;) {
		usb_service(host_byte);
		buttons_poll(button_event);

		int j = job;
		__mem_fence_acquire();
		if (j == JOB_FRAME) {
			if (!sending) {
				lcd_frame_begin(job_top, job_tail, job_tail_rows, job_pal);
				video_begin(job_top, job_tail, job_tail_rows, job_pal);
				sending = true;
				t0 = time_us_64();
			}
			bool done = lcd_frame_step();
			video_rows(lcd_rows_read());
			__mem_fence_release();
			job_rows_read = lcd_rows_read();
			if (done) {
				sending = false;
				lcd_frame_us = (uint32_t)(time_us_64() - t0);
				if (hud_ready) draw_hud();
				__mem_fence_release();
				job = JOB_NONE;
			}
		} else if (j == JOB_MESSAGE) {
			lcd_message(job_title, job_msg);
			__mem_fence_release();
			job = JOB_NONE;
		}
	}
}

void io_start(void)
{
	multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
}
