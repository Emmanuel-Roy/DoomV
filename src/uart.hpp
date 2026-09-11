#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

// Minimal 8250/16550-compatible UART -- just enough register behavior for
// OpenSBI's own driver (tools/linux/opensbi/src/lib/utils/serial/uart8250.c) to
// treat it as a real console. Register offsets/defaults (reg-shift=0,
// reg-io-width=1, reg-offset=0 -- confirmed against fdt_helper.c's
// DEFAULT_UART_REG_* constants) mean this needs no DTS overrides.
//
// TX (THR write) reuses the same host-stdout putchar behavior
// Memory::MMIO_DEBUG already has. RX is a small mutex-protected byte ring,
// fed from the GUI thread's keyboard polling when in Linux-boot mode (see
// DoomSystem::translate_console_key) -- same cross-thread shape as
// Memory::key_queue.
//
// LSR's THRE/TEMT bits must always read as 1: uart8250_putc() spins on
// THRE before every byte, so reporting "busy" here would hang OpenSBI's
// own boot exactly the way the missing misa CSR did in Stage 3.
class Uart {
public:
	Uart();

	uint8_t read(uint64_t offset);
	void write(uint64_t offset, uint8_t val);

	void push_rx(uint8_t byte);
	// Same, but says whether the byte fit. A keyboard wants the dropping
	// version -- a human cannot outrun a 16-byte ring, and if they could,
	// stalling the GUI thread would be the wrong answer. A pipe can outrun
	// it trivially, so the headless stdin feed needs to know and wait.
	bool try_push_rx(uint8_t byte);

	// Watch the transmit side for a string, so an input feed can wait for
	// the guest to ask before answering. Input sent before the guest's tty
	// exists is not queued anywhere -- it is read out of this ring by
	// OpenSBI, handed to a console that has no line discipline yet, and
	// dropped -- so a pipe that starts talking at reset loses its first
	// bytes. Waiting on a prompt is the only correct way to avoid that; a
	// delay is a guess that gets longer every time the guest gets slower.
	void expect(const char *needle);
	bool expect_seen() const { return expect_hit; }

private:
	static constexpr int RX_RING_SIZE = 16;

	// Matched incrementally against the transmit byte stream, so the
	// needle is found even when it straddles two writes -- which it always
	// will, since the guest writes one character per store.
	std::string expect_needle;
	size_t expect_pos = 0;
	std::atomic<bool> expect_hit{true}; // no needle set == already satisfied

	uint8_t rx_ring[RX_RING_SIZE];
	int rx_head;
	int rx_tail;
	std::mutex rx_mutex;

	// IER/FCR/LCR/MCR/SCR: OpenSBI's init sequence writes these but never
	// depends on the values sticking -- plain read-back-last-value storage.
	uint8_t ier;
	uint8_t fcr;
	uint8_t lcr;
	uint8_t mcr;
	uint8_t scr;
};
