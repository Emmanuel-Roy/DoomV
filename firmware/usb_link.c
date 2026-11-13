// USB, run by core 1 (core1_io.c), so DOOM on core 0 never waits for the host.
//
// Core 0 writes into two single-producer/single-consumer rings; core 1 calls
// usb_service() in its loop, which runs TinyUSB's task and moves bytes from
// the rings into the two CDC interfaces. Bytes the PC sends on the link port
// go straight to core 1's command parser. Each ring has exactly one writer
// and one reader, so the indices need only a memory fence, not a lock.

#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

#include "usb_link.h"

enum { CDC_CONSOLE = 0, CDC_LINK = 1 };

typedef struct {
	uint8_t *buf;
	uint32_t size;               // power of two
	volatile uint32_t head;      // next byte to write (producer: core 0)
	volatile uint32_t tail;      // next byte to read (consumer: core 1)
} ring_t;

static uint8_t console_buf[1024], link_buf[2048];
static ring_t console = { console_buf, sizeof console_buf, 0, 0 };
static ring_t link = { link_buf, sizeof link_buf, 0, 0 };

static volatile bool link_open;

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
		if (!used) break;
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

void usb_init(void)
{
	tud_init(0);   // on core 1: USB interrupts now belong to it
}

void usb_service(void (*host_byte)(uint8_t c))
{
	tud_task();
	link_open = tud_cdc_n_connected(CDC_LINK);
	if (tud_cdc_n_connected(CDC_CONSOLE)) drain(&console, CDC_CONSOLE);
	else console.tail = console.head;          // nobody listening: discard
	if (link_open) drain(&link, CDC_LINK);
	uint8_t c;
	while (tud_cdc_n_available(CDC_LINK) && tud_cdc_n_read(CDC_LINK, &c, 1) == 1) host_byte(c);
}

void usb_console_write(const char *s, size_t n)
{
	if (ring_free(&console) < n) return;     // drop rather than stall DOOM
	ring_put(&console, (const uint8_t *)s, (uint32_t)n);
}

void usb_link_write(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint64_t gone_since = 0;
	while (n) {
		// A host opening the port toggles DTR, which reads as "disconnected"
		// for a moment: dropping then lost the recording's first records
		// (seen on Windows). Wait out short gaps; only a host gone for
		// 2 s has really gone, and there is nothing left to keep in step with.
		if (!link_open) {
			if (!gone_since) gone_since = time_us_64();
			if (time_us_64() - gone_since > 2000000) return;
			tight_loop_contents();
			continue;
		}
		gone_since = 0;
		uint32_t room = ring_free(&link);
		if (!room) { tight_loop_contents(); continue; }   // lock-step: never drop
		uint32_t k = n < room ? (uint32_t)n : room;
		ring_put(&link, b, k);
		b += k;
		n -= k;
	}
}

bool usb_link_connected(void) { return link_open; }

void usb_flush(unsigned timeout_ms)
{
	absolute_time_t until = make_timeout_time_ms(timeout_ms);
	while ((ring_used(&console) || (link_open && ring_used(&link))) && !time_reached(until))
		tight_loop_contents();
}
