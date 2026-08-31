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
		break;
	case UART_IER: ier = val; break;
	case UART_FCR: fcr = val; break;
	case UART_LCR: lcr = val; break;
	case UART_MCR: mcr = val; break;
	case UART_SCR: scr = val; break;
	default: break;
	}
}

void Uart::push_rx(uint8_t byte)
{
	std::lock_guard<std::mutex> lock(rx_mutex);
	int next = (rx_tail + 1) % RX_RING_SIZE;
	if (next == rx_head) return; // ring full, drop the byte
	rx_ring[rx_tail] = byte;
	rx_tail = next;
}
