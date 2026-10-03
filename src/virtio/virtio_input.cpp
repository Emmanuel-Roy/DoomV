#include "virtio_input.hpp"
#include "aplic.hpp"
#include "memory.hpp"
#include <cstring>

namespace {
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
		// The pointer is absolute: ABS_X/ABS_Y in framebuffer pixels, so
		// the guest's cursor sits exactly where the host pointer is over
		// the display, the way QEMU's tablet does it. A relative mouse
		// cannot do that -- the guest integrates deltas under its own
		// acceleration, and the two cursors drift apart the moment the
		// host pointer leaves the display or the guest drops an event.
		// The wheels stay relative; there is no absolute wheel.
		if (cfg_subsel == EV_REL) {
			set_bit(out, size, REL_WHEEL);
			set_bit(out, size, REL_HWHEEL);
			return size;
		}
		if (cfg_subsel == EV_ABS) {
			set_bit(out, size, ABS_X);
			set_bit(out, size, ABS_Y);
			return size;
		}
		if (cfg_subsel == EV_SYN) { set_bit(out, size, SYN_REPORT); return size; }
		return 0;
	case CFG_ABS_INFO: {
		// struct virtio_input_absinfo: min, max, fuzz, flat, res, all LE32.
		// The range is the framebuffer's, which is also the X screen's, so
		// the guest maps a coordinate to a pixel with no scaling at all.
		if (kind != Kind::Mouse) return 0;
		uint32_t max = 0;
		if (cfg_subsel == ABS_X) max = (uint32_t)Memory::LFB_W - 1;
		else if (cfg_subsel == ABS_Y) max = (uint32_t)Memory::LFB_H - 1;
		else return 0;
		std::memcpy(out + 4, &max, sizeof(max));
		return 20;
	}
	default:
		// ID_SERIAL, PROP_BITS: nothing to say. Size zero is a valid
		// answer and the driver skips the feature.
		return 0;
	}
}

// The config space is a window read a byte at a time (a packed struct of
// u8s and a union): select, subsel, size, five reserved bytes, then the
// payload the select/subsel pair asks for.
uint8_t VirtioInput::config_read8(uint64_t o) const
{
	if (o == 0) return cfg_select;
	if (o == 1) return cfg_subsel;
	if (o >= 0x88) return 0;
	uint8_t payload[128];
	const uint8_t size = config_payload(payload);
	if (o == 2) return size;
	if (o < 8) return 0;                  // reserved[5]
	return payload[o - 8];
}

// Only select and subsel are the driver's to write; the rest is
// device-to-driver, and writes to it are dropped as a real device would.
void VirtioInput::config_write8(uint64_t o, uint8_t value)
{
	if (o == 0) cfg_select = value;
	if (o == 1) cfg_subsel = value;
}

// The driver has posted buffers. For the eventq that means there is now
// somewhere to put events, so try; for the statusq it means the driver sent
// something, which is consumed and ignored (LED state, on a keyboard with no
// LEDs).
void VirtioInput::notify(unsigned q, Memory &mem, Aplic &aplic)
{
	if (q == 0) pump(mem, aplic);
	else drain_statusq(mem, aplic);
}

void VirtioInput::drain_statusq(Memory &mem, Aplic &aplic)
{
	// Accept and discard. A status message is the driver telling the
	// device something about itself (keyboard LEDs, almost always); not
	// returning the buffer would leak the driver's ring one entry at a
	// time until it stalled.
	bool completed = false;
	uint16_t head;
	while (next_chain(mem, 1, head)) {
		complete(mem, 1, head, 0);
		completed = true;
	}
	if (completed) interrupt(aplic);
}

void VirtioInput::pump(Memory &mem, Aplic &aplic)
{
	if (!pending.load(std::memory_order_relaxed)) return;
	if (!queues[0].desc) return;

	bool completed = false;
	std::lock_guard<std::mutex> lock(event_mutex);
	// One posted buffer per event, and stop when either runs out. Events
	// that do not fit stay queued: the driver reposts buffers as it
	// consumes them, so the backlog drains on the next notify rather than
	// being lost.
	uint16_t head;
	while (!events.empty() && next_chain(mem, 0, head)) {
		const Desc d = descriptor(mem, 0, head);
		// The buffer has to be device-writable and hold a whole event. A
		// descriptor that is not is a driver bug; it is handed back with a
		// zero length rather than retried, which would spin forever.
		uint32_t written = 0;
		if ((d.flags & DESC_F_WRITE) && d.len >= 8) {
			const Event ev = events.front();
			events.pop_front();
			mem.write8(d.addr + 0, (uint8_t)(ev.type & 0xFF));
			mem.write8(d.addr + 1, (uint8_t)(ev.type >> 8));
			mem.write8(d.addr + 2, (uint8_t)(ev.code & 0xFF));
			mem.write8(d.addr + 3, (uint8_t)(ev.code >> 8));
			mem.write32(d.addr + 4, ev.value);
			written = 8;
		}
		complete(mem, 0, head, written);
		completed = true;
	}
	if (events.empty()) pending.store(false, std::memory_order_relaxed);
	if (completed) interrupt(aplic);
}
