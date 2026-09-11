#include "uart.hpp"
#include <cstdio>

namespace {
constexpr uint64_t UART_RBR_THR = 0; // In: Receive Buffer / Out: Transmit Holding
constexpr uint64_t UART_IER     = 1;
constexpr uint64_t UART_FCR     = 2; // IIR on read, not distinguished here -- nothing reads it
constexpr uint64_t UART_LCR     = 3;
constexpr uint64_t UART_MCR     = 4;
constexpr uint64_t UART_LSR     = 5;
constexpr uint64_t UART_MSR     = 6;
constexpr uint64_t UART_SCR     = 7;

constexpr uint8_t LSR_TEMT = 0x40;
constexpr uint8_t LSR_THRE = 0x20;
constexpr uint8_t LSR_DR   = 0x01;
}

Uart::Uart() : rx_head(0), rx_tail(0), ier(0), fcr(0), lcr(0), mcr(0), scr(0)
{
}

uint8_t Uart::read(uint64_t offset)
{
	switch (offset) {
	case UART_RBR_THR: {
		std::lock_guard<std::mutex> lock(rx_mutex);
		if (rx_head == rx_tail) return 0;
		uint8_t val = rx_ring[rx_head];
		rx_head = (rx_head + 1) % RX_RING_SIZE;
		return val;
	}
	case UART_IER: return ier;
	case UART_LCR: return lcr;
	case UART_MCR: return mcr;
	case UART_LSR: {
		std::lock_guard<std::mutex> lock(rx_mutex);
		uint8_t lsr = LSR_TEMT | LSR_THRE;
		if (rx_head != rx_tail) lsr |= LSR_DR;
		return lsr;
	}
	case UART_MSR: return 0;
	case UART_SCR: return scr;
	default:       return 0;
	}
}

void Uart::write(uint64_t offset, uint8_t val)
{
	switch (offset) {
	case UART_RBR_THR:
		std::putchar(val);
		std::fflush(stdout);
		if (!expect_hit) {
			// Plain incremental match. Restarting at 0 rather than doing
			// the KMP fallback costs nothing here and cannot miss a
			// prompt: console prompts are not self-overlapping strings.
			if (val == (uint8_t)expect_needle[expect_pos]) {
				if (++expect_pos == expect_needle.size()) expect_hit = true;
			} else {
				expect_pos = (val == (uint8_t)expect_needle[0]) ? 1 : 0;
			}
		}
		break;
	case UART_IER: ier = val; break;
	case UART_FCR: fcr = val; break;
	case UART_LCR: lcr = val; break;
	case UART_MCR: mcr = val; break;
	case UART_SCR: scr = val; break;
	default: break;
	}
}

void Uart::expect(const char *needle)
{
	expect_needle = needle ? needle : "";
	expect_pos = 0;
	expect_hit = expect_needle.empty();
}

void Uart::push_rx(uint8_t byte)
{
	(void)try_push_rx(byte);
}

bool Uart::try_push_rx(uint8_t byte)
{
	std::lock_guard<std::mutex> lock(rx_mutex);
	int next = (rx_tail + 1) % RX_RING_SIZE;
	if (next == rx_head) return false; // ring full
	rx_ring[rx_tail] = byte;
	rx_tail = next;
	return true;
}
