#pragma once
// virtio-net over the MMIO transport: an Ethernet card.
//
// Two queues. The guest posts empty buffers on receiveq (0) and the device
// fills one per incoming frame; it posts frames to send on transmitq (1) and
// the device hands each to whatever is behind it -- src/net/usernet.cpp, a
// user-mode NAT, or nothing at all. Every frame, both ways, is preceded by the
// 12-byte virtio_net_hdr; no offloads are offered, so the header is all zero
// but for num_buffers, and frames are plain Ethernet of at most 1514 bytes.
//
// Frames reach the guest only through receive(), which DoomSystem calls at
// the same instruction counts as keyboard input: what arrives from the host
// network is outside the machine, like typing, and this is how it is made to
// land at a reproducible point -- and how -record and -replay capture it.
#include "virtio_mmio.hpp"
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

class VirtioNet final : public VirtioMmio {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr uint32_t DEVICE_ID = 1;           // network card
	static constexpr size_t HEADER = 12;               // virtio_net_hdr_v1
	static constexpr size_t MAX_FRAME = 1514;          // Ethernet, no FCS
	static constexpr uint8_t MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

	explicit VirtioNet(uint32_t irq) : VirtioMmio(DEVICE_ID, VENDOR_DOOM, irq, 2) {}

	// A card is only there when the machine is given a network (-net); an
	// empty slot answers device id 0, as an empty drive slot does.
	void set_connected(bool on) { connected = on; }
	bool is_connected() const { return connected; }

	// Where frames the guest sends go. Unset, they are dropped -- a replay
	// has no network behind it, only the frames that came in.
	std::function<void(const uint8_t *frame, size_t len)> on_transmit;

	// A frame for the guest. Queued, then written into the next buffer the
	// guest has posted; delivered as soon as there is one.
	void receive(std::vector<uint8_t> frame, Memory &mem, Aplic &aplic);
	// Deliver what is queued into the buffers the guest has posted.
	void pump(Memory &mem, Aplic &aplic);

private:
	bool present() const override { return connected; }
	// Word 0: VIRTIO_NET_F_MAC (bit 5), so the guest takes the address in
	// config space. Word 1: VIRTIO_F_VERSION_1.
	uint32_t features(uint32_t sel) const override { return sel == 0 ? (1u << 5) : sel == 1 ? 1u : 0u; }
	// struct virtio_net_config begins with mac[6].
	uint8_t config_read8(uint64_t offset) const override { return offset < 6 ? MAC[offset] : 0; }
	void notify(unsigned q, Memory &mem, Aplic &aplic) override;
	// Frames waiting for a reset driver are for a driver that is gone.
	void reset() override { rx_pending.clear(); }

	void transmit(Memory &mem, Aplic &aplic);

	bool connected = false;
	// Frames for the guest that have no buffer yet. Bounded: past it the
	// oldest goes, as a real card's FIFO overflows.
	static constexpr size_t RX_LIMIT = 4096;
	std::deque<std::vector<uint8_t>> rx_pending;
	std::vector<uint8_t> tx_buf;
};
