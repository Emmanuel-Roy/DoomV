#pragma once
#include <cstdint>
#include <mutex>

// Minimal 8250/16550-compatible UART -- just enough register behavior for
// OpenSBI's own driver (tools/opensbi/src/lib/utils/serial/uart8250.c) to
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

private:
	static constexpr int RX_RING_SIZE = 16;

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
