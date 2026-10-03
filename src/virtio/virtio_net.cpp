#include "virtio_net.hpp"
#include "memory.hpp"
#include <algorithm>

void VirtioNet::receive(std::vector<uint8_t> frame, Memory &mem, Aplic &aplic)
{
	if (!connected || frame.size() > MAX_FRAME) return;
	if (rx_pending.size() >= RX_LIMIT) rx_pending.pop_front();
	rx_pending.push_back(std::move(frame));
	pump(mem, aplic);
}

void VirtioNet::notify(unsigned q, Memory &mem, Aplic &aplic)
{
	if (q == 0) pump(mem, aplic);       // new receive buffers
	else transmit(mem, aplic);
}

// One frame per posted chain: the header, then the frame, laid across the
// chain's device-writable descriptors in order. A chain too small for the
// frame gets what fits -- a driver bug, and the guest drops a short frame.
void VirtioNet::pump(Memory &mem, Aplic &aplic)
{
	if (rx_pending.empty() || !queues[0].desc) return;
	bool completed = false;
	uint16_t head;
	while (!rx_pending.empty() && next_chain(mem, 0, head)) {
		const std::vector<uint8_t> &frame = rx_pending.front();
		uint8_t header[HEADER] = {};
		header[10] = 1;   // num_buffers: one chain holds the whole frame
		size_t at = 0;    // bytes of header + frame written so far
		const size_t total = HEADER + frame.size();
		uint16_t d = head;
		for (uint32_t guard = 0; guard <= queue_size(queues[0]) && at < total; guard++) {
			const Desc desc = descriptor(mem, 0, d);
			if (desc.flags & DESC_F_WRITE) {
				size_t room = desc.len, off = 0;
				while (room > 0 && at < total) {
					const uint8_t *src = at < HEADER ? header + at : frame.data() + (at - HEADER);
					const size_t n = std::min(room, at < HEADER ? HEADER - at : total - at);
					mem.write_bytes(desc.addr + off, src, n);
					at += n; off += n; room -= n;
				}
			}
			if (!(desc.flags & DESC_F_NEXT)) break;
			d = desc.next;
		}
		complete(mem, 0, head, (uint32_t)at);
		rx_pending.pop_front();
		completed = true;
	}
	if (completed) interrupt(aplic);
}

// Each chain is a header and a frame in device-readable buffers. The frame is
// handed on and the chain returned at once: a card that has sent a frame is
// done with it, whatever becomes of the frame afterwards.
void VirtioNet::transmit(Memory &mem, Aplic &aplic)
{
	if (!queues[1].desc) return;
	bool completed = false;
	uint16_t head;
	while (next_chain(mem, 1, head)) {
		tx_buf.clear();
		uint16_t d = head;
		for (uint32_t guard = 0; guard <= queue_size(queues[1]); guard++) {
			const Desc desc = descriptor(mem, 1, d);
			if (!(desc.flags & DESC_F_WRITE) && tx_buf.size() + desc.len <= HEADER + MAX_FRAME) {
				const size_t at = tx_buf.size();
				tx_buf.resize(at + desc.len);
				mem.read_bytes(desc.addr, tx_buf.data() + at, desc.len);
			}
			if (!(desc.flags & DESC_F_NEXT)) break;
			d = desc.next;
		}
		if (tx_buf.size() > HEADER && on_transmit) on_transmit(tx_buf.data() + HEADER, tx_buf.size() - HEADER);
		complete(mem, 1, head, 0);
		completed = true;
	}
	if (completed) interrupt(aplic);
}
