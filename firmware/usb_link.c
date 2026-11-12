// USB lives on core 1, so DOOM on core 0 never waits for the host.
//
// Core 0 writes into two single-producer/single-consumer rings; core 1 runs
// TinyUSB's task loop and moves bytes from the rings into the two CDC
// interfaces, and bytes the host sends on the link port into a third ring.
// Each ring has exactly one writer and one reader, so the head and tail
// indices need only a memory fence, not a lock.
//
// Core 1 runs on a small stack in main RAM, which leaves SCRATCH_X free for
// core 0's stack to grow into (see firmware/CMakeLists.txt).

#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "tusb.h"

#include "usb_link.h"

enum { CDC_CONSOLE = 0, CDC_LINK = 1 };

typedef struct {
	uint8_t *buf;
	uint32_t size;               // power of two
	volatile uint32_t head;      // next byte to write (producer)
	volatile uint32_t tail;      // next byte to read (consumer)
} ring_t;

static uint8_t console_buf[1024], link_buf[2048], rx_buf[256];
static ring_t console = { console_buf, sizeof console_buf, 0, 0 };
static ring_t link = { link_buf, sizeof link_buf, 0, 0 };
static ring_t rx = { rx_buf, sizeof rx_buf, 0, 0 };

static volatile bool link_open;
static uint32_t core1_stack[256];   // 1 KB is plenty for TinyUSB's task

static uint32_t ring_used(const ring_t *r) { return r->head - r->tail; }
static uint32_t ring_free(const ring_t *r) { return r->size - ring_used(r); }

static void ring_put(ring_t *r, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) r->buf[(r->head + i) & (r->size - 1)] = p[i];
	__mem_fence_release();
	r->head += n;
}

// Move what fits from a ring into a CDC interface's FIFO.
static void drain(ring_t *r, int itf)
{
	for (;;) {
		uint32_t used = ring_used(r);
		if (!used) return;
		__mem_fence_acquire();
		uint32_t start = r->tail & (r->size - 1);
		uint32_t chunk = used < r->size - start ? used : r->size - start;
		uint32_t room = tud_cdc_n_write_available(itf);
		if (!room) break;
		if (chunk > room) chunk = room;
		uint32_t wrote = tud_cdc_n_write(itf, r->buf + start, chunk);
		__mem_fence_release();
		r->tail += wrote;
		if (wrote < chunk) break;
	}
	tud_cdc_n_write_flush(itf);
}

static void core1_main(void)
{
	tud_init(0);   // USB interrupts now belong to core 1
	for (;;) {
		tud_task();
		link_open = tud_cdc_n_connected(CDC_LINK);
		if (tud_cdc_n_connected(CDC_CONSOLE)) drain(&console, CDC_CONSOLE);
		else console.tail = console.head;          // nobody listening: discard
		if (link_open) drain(&link, CDC_LINK);
		while (tud_cdc_n_available(CDC_LINK) && ring_free(&rx)) {
			uint8_t c;
			if (tud_cdc_n_read(CDC_LINK, &c, 1) == 1) ring_put(&rx, &c, 1);
		}
	}
}

void usb_start(void)
{
	multicore_launch_core1_with_stack(core1_main, core1_stack, sizeof core1_stack);
}

void usb_console_write(const char *s, size_t n)
{
	if (ring_free(&console) < n) return;     // drop rather than stall DOOM
	ring_put(&console, (const uint8_t *)s, (uint32_t)n);
}

void usb_link_write(const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		if (!link_open) return;            // host went away: nothing to keep in step with
		uint32_t room = ring_free(&link);
		if (!room) { tight_loop_contents(); continue; }   // lock-step: never drop
		uint32_t k = n < room ? (uint32_t)n : room;
		ring_put(&link, b, k);
		b += k;
		n -= k;
	}
}

bool usb_link_connected(void) { return link_open; }

int usb_link_getc(void)
{
	if (!ring_used(&rx)) return -1;
	__mem_fence_acquire();
	int c = rx.buf[rx.tail & (rx.size - 1)];
	__mem_fence_release();
	rx.tail++;
	return c;
}

void usb_flush(unsigned timeout_ms)
{
	absolute_time_t until = make_timeout_time_ms(timeout_ms);
	while ((ring_used(&console) || (link_open && ring_used(&link))) && !time_reached(until))
		tight_loop_contents();
}
