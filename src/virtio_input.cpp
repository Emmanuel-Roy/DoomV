#include "virtio_input.hpp"
#include "aplic.hpp"
#include "memory.hpp"
#include <cstring>

namespace {
constexpr uint16_t DESC_F_NEXT  = 1;
constexpr uint16_t DESC_F_WRITE = 2;
constexpr uint64_t DESC_SIZE    = 16;

// Memory has no write16, so the one genuinely 16-bit field a device
// publishes -- the used ring's index -- goes out as two bytes. Same helper
// as virtio_blk's, and for the same reason.
void write16_phys(Memory &mem, uint64_t addr, uint16_t v)
{
	mem.write8(addr, (uint8_t)(v & 0xFF));
	mem.write8(addr + 1, (uint8_t)(v >> 8));
}

// Set bit `n` in a little-endian bitmap, growing the reported size to cover
// it. This is how EV_BITS and the per-type code bitmaps are described: the
// driver reads `size` bytes and treats them as a bitmap of codes.
void set_bit(uint8_t *bitmap, uint8_t &size, unsigned n)
{
	bitmap[n / 8] |= (uint8_t)(1u << (n % 8));
	const uint8_t needed = (uint8_t)(n / 8 + 1);
	if (needed > size) size = needed;
}
}

void VirtioInput::push(uint16_t type, uint16_t code, uint32_t value)
{
	std::lock_guard<std::mutex> lock(event_mutex);
	// A bound, because the producer is a window and the consumer is an
	// emulated machine: hold a key down during a slow boot and the guest
	// may not drain anything for whole seconds. Dropping the oldest is the
	// right end to drop from -- what a person wants delivered is what they
	// just did, not what they did four seconds ago.
	if (events.size() >= 4096) events.pop_front();
	events.push_back({type, code, value});
	pending.store(true, std::memory_order_relaxed);
}

void VirtioInput::sync()
{
	push(EV_SYN, SYN_REPORT, 0);
}

uint8_t VirtioInput::config_payload(uint8_t out[128]) const
{
	std::memset(out, 0, 128);
	uint8_t size = 0;

	switch (cfg_select) {
	case CFG_ID_NAME: {
		// Shown in dmesg and by evtest. Naming them distinctly is worth
		// the two lines: "input: DoomV Keyboard as /devices/..." is how
		// you tell at a glance that the guest bound both devices.
		const char *name = (kind == Kind::Keyboard) ? "DoomV Keyboard" : "DoomV Mouse";
		size = (uint8_t)std::strlen(name);
		std::memcpy(out, name, size);
		return size;
	}
	case CFG_ID_DEVIDS:
		// bustype, vendor, product, version -- four LE16s. BUS_VIRTUAL
		// (0x06), because that is what this is; claiming BUS_USB would
		// invite udev rules written for real hardware to match it.
		out[0] = 0x06;
		size = 8;
		return size;
	case CFG_EV_BITS:
		// The important one. `subsel` is the event type being asked
		// about, and the payload is a bitmap of the codes of that type
		// this device can produce. Answering nothing here gets you a
		// device that probes fine and is routed no input at all.
		if (kind == Kind::Keyboard) {
			if (cfg_subsel == EV_KEY) {
				// Codes 1..127 -- the whole classic AT keyboard, which
				// is exactly the range gui.cpp's scancode table maps
				// into. Declaring the range rather than only the keys
				// the table happens to fill means adding a key later is
				// a one-line change in one place.
				for (unsigned n = 1; n <= 127; n++) set_bit(out, size, n);
				return size;
			}
			if (cfg_subsel == EV_SYN) { set_bit(out, size, SYN_REPORT); return size; }
			return 0;
		}
		if (cfg_subsel == EV_KEY) {
			set_bit(out, size, BTN_LEFT);
			set_bit(out, size, BTN_RIGHT);
			set_bit(out, size, BTN_MIDDLE);
			return size;
		}
		if (cfg_subsel == EV_REL) {
			set_bit(out, size, REL_X);
			set_bit(out, size, REL_Y);
			set_bit(out, size, REL_WHEEL);
			set_bit(out, size, REL_HWHEEL);
			return size;
		}
		if (cfg_subsel == EV_SYN) { set_bit(out, size, SYN_REPORT); return size; }
		return 0;
	default:
		// ID_SERIAL, PROP_BITS, ABS_INFO: nothing to say. Size zero is a
		// valid answer and the driver skips the feature.
		return 0;
	}
}

uint8_t VirtioInput::read8(uint64_t offset) const
{
	// The config space is read a byte at a time by the driver (it is a
	// packed struct of u8s and a union), which is why this device needs
	// byte access at all where virtio-blk only ever needed words.
	if (offset >= REG_CONFIG && offset < REG_CONFIG + 0x88) {
		const uint64_t o = offset - REG_CONFIG;
		if (o == 0) return cfg_select;
		if (o == 1) return cfg_subsel;
		uint8_t payload[128];
		const uint8_t size = config_payload(payload);
		if (o == 2) return size;
		if (o < 8) return 0;                  // reserved[5]
		const uint64_t i = o - 8;
		return (i < 128) ? payload[i] : 0;
	}
	// Everything else is a 32-bit transport register; serve a byte out of
	// it rather than returning zero, so a driver that reads narrowly gets
	// the truth.
	const uint32_t word = read32(offset & ~3ull);
	return (uint8_t)(word >> (8 * (offset & 3)));
}

uint32_t VirtioInput::read32(uint64_t offset) const
{
	switch (offset) {
	case REG_MAGIC:     return MAGIC;
	case REG_VERSION:   return VERSION;
	case REG_DEVICE_ID: return DEVICE_ID;
	case REG_VENDOR_ID: return 0x554d4551; // "QEMU", the conventional value
	// No features at all, in either word. VIRTIO_F_VERSION_1 lives at bit
	// 32 and the MMIO version-2 transport implies it, so there is nothing
	// this device needs to negotiate.
	case REG_DEVICE_FEAT:
		return (device_feat_sel == 1) ? 1u : 0u; // bit 32: VIRTIO_F_VERSION_1
	case REG_QUEUE_NUM_MAX:
		return (queue_sel < NUM_QUEUES) ? QUEUE_MAX : 0;
	case REG_QUEUE_READY:
		return (queue_sel < NUM_QUEUES) ? queues[queue_sel].ready : 0;
	case REG_INTERRUPT_STAT: return interrupt_status;
	case REG_STATUS:         return status;
	case REG_CONFIG_GEN:     return 0;
	default:
		if (offset >= REG_CONFIG) {
			// A 32-bit read of config space, assembled from the byte view
			// so there is one definition of what config space contains.
			uint32_t v = 0;
			for (int i = 0; i < 4; i++) v |= (uint32_t)read8(offset + i) << (8 * i);
			return v;
		}
		return 0;
	}
}

void VirtioInput::write8(uint64_t offset, uint8_t value)
{
	if (offset == REG_CONFIG + 0) { cfg_select = value; return; }
	if (offset == REG_CONFIG + 1) { cfg_subsel = value; return; }
	// The rest of config space is device-to-driver. Writes to it are the
	// driver's business to not make, and dropping them is what a real
	// device does with a read-only register.
}

void VirtioInput::write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic)
{
	switch (offset) {
	case REG_DEVICE_FEAT_SEL: device_feat_sel = value; break;
	// The driver's half of feature negotiation is accepted and dropped:
	// this device offers nothing but VIRTIO_F_VERSION_1, which the version-2
	// MMIO transport implies, so there is no state worth keeping.
	case REG_DRIVER_FEAT_SEL: break;
	case REG_DRIVER_FEAT:     break;
	case REG_QUEUE_SEL:       queue_sel = value; break;
	case REG_QUEUE_NUM:
		if (queue_sel < NUM_QUEUES) queues[queue_sel].num = value;
		break;
	case REG_QUEUE_READY:
		if (queue_sel < NUM_QUEUES) queues[queue_sel].ready = value;
		break;
	case REG_QUEUE_DESC_LO:
		if (queue_sel < NUM_QUEUES)
			queues[queue_sel].desc = (queues[queue_sel].desc & ~0xFFFFFFFFull) | value;
		break;
	case REG_QUEUE_DESC_HI:
		if (queue_sel < NUM_QUEUES)
			queues[queue_sel].desc = (queues[queue_sel].desc & 0xFFFFFFFFull) | ((uint64_t)value << 32);
		break;
	case REG_QUEUE_AVAIL_LO:
		if (queue_sel < NUM_QUEUES)
			queues[queue_sel].avail = (queues[queue_sel].avail & ~0xFFFFFFFFull) | value;
		break;
	case REG_QUEUE_AVAIL_HI:
		if (queue_sel < NUM_QUEUES)
			queues[queue_sel].avail = (queues[queue_sel].avail & 0xFFFFFFFFull) | ((uint64_t)value << 32);
		break;
	case REG_QUEUE_USED_LO:
		if (queue_sel < NUM_QUEUES)
			queues[queue_sel].used = (queues[queue_sel].used & ~0xFFFFFFFFull) | value;
		break;
	case REG_QUEUE_USED_HI:
		if (queue_sel < NUM_QUEUES)
			queues[queue_sel].used = (queues[queue_sel].used & 0xFFFFFFFFull) | ((uint64_t)value << 32);
		break;
	case REG_QUEUE_NOTIFY:
		// The driver has posted buffers. For the eventq that means there
		// is now somewhere to put events, so try; for the statusq it
		// means the driver sent us something, which is consumed and
		// ignored (LED state, on a keyboard with no LEDs).
		if (value == 0) pump(mem, aplic);
		else if (value == 1) drain_statusq(mem, aplic);
		break;
	case REG_INTERRUPT_ACK:
		interrupt_status &= ~value;
		break;
	case REG_STATUS:
		status = value;
		// Status 0 is a reset. Everything the driver told us is now
		// stale, including how far into the avail rings we had read --
		// leaving last_avail behind across a reset is how a re-probed
		// device appears to hang.
		if (value == 0) {
			for (unsigned q = 0; q < NUM_QUEUES; q++) queues[q] = Queue{};
			interrupt_status = 0;
			cfg_select = cfg_subsel = 0;
		}
		break;
	default:
		if (offset >= REG_CONFIG) write8(offset, (uint8_t)value);
		break;
	}
}

void VirtioInput::drain_statusq(Memory &mem, Aplic &aplic)
{
	Queue &q = queues[1];
	if (!q.ready || !q.avail || !q.used) return;
	const uint32_t qsz = q.num ? q.num : QUEUE_MAX;
	const uint16_t avail_idx = mem.read16(q.avail + 2);
	bool completed = false;

	// Accept and discard. A status message is the driver telling the
	// device something about itself (keyboard LEDs, almost always); not
	// returning the buffer would leak the driver's ring one entry at a
	// time until it stalled.
	while (q.last_avail != avail_idx) {
		const uint16_t slot = (uint16_t)(q.last_avail % qsz);
		const uint16_t head = mem.read16(q.avail + 4 + (uint64_t)slot * 2);
		const uint16_t used_idx  = mem.read16(q.used + 2);
		const uint64_t slot_addr = q.used + 4 + (uint64_t)(used_idx % qsz) * 8;
		mem.write32(slot_addr, head);
		mem.write32(slot_addr + 4, 0);
		write16_phys(mem, q.used + 2, (uint16_t)(used_idx + 1));
		q.last_avail++;
		completed = true;
	}

	if (completed) {
		interrupt_status |= 1;
		aplic.assert_source(irq);
	}
}

void VirtioInput::pump(Memory &mem, Aplic &aplic)
{
	if (!pending.load(std::memory_order_relaxed)) return;

	Queue &q = queues[0];
	if (!q.ready || !q.avail || !q.used || !q.desc) return;

	const uint32_t qsz = q.num ? q.num : QUEUE_MAX;
	const uint16_t avail_idx = mem.read16(q.avail + 2);
	bool completed = false;

	std::lock_guard<std::mutex> lock(event_mutex);
	// One posted buffer per event, and stop when either runs out. Events
	// that do not fit stay queued: the driver reposts buffers as it
	// consumes them, so the backlog drains on the next notify rather than
	// being lost.
	while (!events.empty() && q.last_avail != avail_idx) {
		const uint16_t slot = (uint16_t)(q.last_avail % qsz);
		const uint16_t head = mem.read16(q.avail + 4 + (uint64_t)slot * 2);
		const uint64_t da    = q.desc + (uint64_t)head * DESC_SIZE;
		const uint64_t addr  = mem.read64(da);
		const uint32_t len   = mem.read32(da + 8);
		const uint16_t flags = mem.read16(da + 12);

		// The buffer has to be device-writable and hold a whole event.
		// Anything else is a driver bug; returning the descriptor with a
		// zero length says "nothing written" rather than corrupting it.
		uint32_t written = 0;
		if ((flags & DESC_F_WRITE) && len >= 8) {
			const Event ev = events.front();
			events.pop_front();
			mem.write8(addr + 0, (uint8_t)(ev.type & 0xFF));
			mem.write8(addr + 1, (uint8_t)(ev.type >> 8));
			mem.write8(addr + 2, (uint8_t)(ev.code & 0xFF));
			mem.write8(addr + 3, (uint8_t)(ev.code >> 8));
			mem.write32(addr + 4, ev.value);
			written = 8;
		}
		// A descriptor that is not writable or is too small is a driver
		// bug. It still gets consumed and handed back with a zero length
		// rather than retried, because retrying it would spin forever.

		const uint16_t used_idx  = mem.read16(q.used + 2);
		const uint64_t slot_addr = q.used + 4 + (uint64_t)(used_idx % qsz) * 8;
		mem.write32(slot_addr, head);
		mem.write32(slot_addr + 4, written);
		// Index last, so a driver that sees the new index always sees a
		// complete entry behind it.
		write16_phys(mem, q.used + 2, (uint16_t)(used_idx + 1));

		q.last_avail++;
		completed = true;
	}

	if (events.empty()) pending.store(false, std::memory_order_relaxed);

	if (completed) {
		interrupt_status |= 1;   // the used ring was updated
		aplic.assert_source(irq);
	}
}
