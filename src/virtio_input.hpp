#pragma once
// virtio-input over the MMIO transport: a keyboard and a mouse the guest
// can actually read.
//
// Until now the only way anything reached a Linux guest was the UART. That
// is a serial line: it carries characters, it has no notion of a key being
// held down, and it is wired to `hvc0` rather than to the machine's console
// as a whole. So the framebuffer console -- the thing the window exists to
// show -- could be watched and never typed at, and there was no pointer at
// all. `console=tty0` got output; input came from somewhere else entirely.
//
// A real machine does not work that way, and neither does anything above a
// serial console: the VT layer takes its keystrokes from an *input device*
// (drivers/tty/vt/keyboard.c registers an input handler), and every pointer
// in existence is one too. So this is a device rather than another special
// case in the UART: with it, tty1 in the window is a console you can log in
// on, and a mouse exists for anything that wants one.
//
// Shape of the device, which is much simpler than virtio-blk's:
//
//   * two queues. Queue 0 (eventq) is the interesting one and runs
//     device-to-driver: the driver posts a pile of empty 8-byte writable
//     buffers, and the device fills one in per event. Queue 1 (statusq) is
//     driver-to-device, for things like keyboard LEDs, and is accepted and
//     discarded here.
//   * an 8-byte event: `{ le16 type; le16 code; le32 value; }` -- the same
//     type/code/value triple as Linux's own `input_event`, minus the
//     timestamp, which the driver fills in on arrival.
//   * a config space that is a *window*, not a struct: the driver writes a
//     select/subsel pair and reads back a size and up to 128 bytes. That is
//     how it asks "what events do you generate?" (EV_BITS) and "what are
//     you called?" (ID_NAME). A device that answers EV_BITS with nothing
//     registers successfully and produces an input device with no
//     capabilities, which nothing will route keystrokes to.
//
// Two instances rather than one combined device, because that is what the
// driver models: virtio_input registers exactly one `input_dev` per virtio
// device. A single device claiming both EV_KEY and EV_REL would be a mouse
// with a hundred buttons as far as the input layer is concerned, and
// `libinput` and the VT layer both make decisions off that classification.
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

class Memory;
class Aplic;

class VirtioInput {
public:
	// The MMIO transport's registers are the same set virtio-blk uses --
	// the transport does not vary by device type, only the device id and
	// the config space do.
	enum : uint64_t {
		REG_MAGIC           = 0x000,
		REG_VERSION         = 0x004,
		REG_DEVICE_ID       = 0x008,
		REG_VENDOR_ID       = 0x00c,
		REG_DEVICE_FEAT     = 0x010,
		REG_DEVICE_FEAT_SEL = 0x014,
		REG_DRIVER_FEAT     = 0x020,
		REG_DRIVER_FEAT_SEL = 0x024,
		REG_QUEUE_SEL       = 0x030,
		REG_QUEUE_NUM_MAX   = 0x034,
		REG_QUEUE_NUM       = 0x038,
		REG_QUEUE_READY     = 0x044,
		REG_QUEUE_NOTIFY    = 0x050,
		REG_INTERRUPT_STAT  = 0x060,
		REG_INTERRUPT_ACK   = 0x064,
		REG_STATUS          = 0x070,
		REG_QUEUE_DESC_LO   = 0x080,
		REG_QUEUE_DESC_HI   = 0x084,
		REG_QUEUE_AVAIL_LO  = 0x090,
		REG_QUEUE_AVAIL_HI  = 0x094,
		REG_QUEUE_USED_LO   = 0x0a0,
		REG_QUEUE_USED_HI   = 0x0a4,
		REG_CONFIG_GEN      = 0x0fc,
		REG_CONFIG          = 0x100,
	};

	static constexpr uint32_t MAGIC     = 0x74726976; // "virt"
	static constexpr uint32_t VERSION   = 2;
	static constexpr uint32_t DEVICE_ID = 18;         // input device
	static constexpr uint32_t QUEUE_MAX = 256;
	static constexpr unsigned NUM_QUEUES = 2;         // eventq, statusq

	// Config space selects, from the spec.
	enum : uint8_t {
		CFG_UNSET     = 0x00,
		CFG_ID_NAME   = 0x01,
		CFG_ID_SERIAL = 0x02,
		CFG_ID_DEVIDS = 0x03,
		CFG_PROP_BITS = 0x10,
		CFG_EV_BITS   = 0x11,
		CFG_ABS_INFO  = 0x12,
	};

	// Linux event types and the codes used here, spelled out rather than
	// pulled from a header the emulator does not get to include.
	enum : uint16_t {
		EV_SYN = 0x00,
		EV_KEY = 0x01,
		EV_REL = 0x02,
	};
	enum : uint16_t {
		SYN_REPORT = 0x00,
		REL_X      = 0x00,
		REL_Y      = 0x01,
		REL_WHEEL  = 0x08,
		REL_HWHEEL = 0x06,
		BTN_LEFT   = 0x110,
		BTN_RIGHT  = 0x111,
		BTN_MIDDLE = 0x112,
	};

	enum class Kind { Keyboard, Mouse };

	explicit VirtioInput(Kind kind, uint32_t irq) : kind(kind), irq(irq) {}

	// Producer side, called from the GUI thread. An event is queued, not
	// delivered: writing to guest memory is the CPU thread's business and
	// the queues are its state. pump() is the consumer.
	void push(uint16_t type, uint16_t code, uint32_t value);
	// EV_SYN/SYN_REPORT, which ends a report. The input layer buffers
	// everything until it sees one, so a press with no SYN after it is a
	// press the guest never hears about.
	void sync();
	bool has_pending() const { return pending.load(std::memory_order_relaxed); }

	uint8_t  read8(uint64_t offset) const;
	uint32_t read32(uint64_t offset) const;
	void write8(uint64_t offset, uint8_t value);
	void write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic);

	// Hand queued events to the guest. Called from the CPU thread often
	// enough that a mouse feels attached -- see the call site for what
	// "often enough" costs.
	void pump(Memory &mem, Aplic &aplic);

private:
	struct Queue {
		uint32_t num = 0;
		uint32_t ready = 0;
		uint64_t desc = 0, avail = 0, used = 0;
		uint16_t last_avail = 0;
	};

	struct Event {
		uint16_t type, code;
		uint32_t value;
	};

	// Accept and discard the driver's status messages (keyboard LEDs, in
	// practice). Not returning those buffers would leak the driver's ring.
	void drain_statusq(Memory &mem, Aplic &aplic);

	// Fills `out` (at most 128 bytes) for the current select/subsel and
	// returns the size the driver should see. Zero means "not supported",
	// which is a legitimate answer the driver handles.
	uint8_t config_payload(uint8_t out[128]) const;

	const Kind kind;
	const uint32_t irq;

	uint32_t status = 0;
	uint32_t device_feat_sel = 0;
	uint32_t queue_sel = 0;
	uint32_t interrupt_status = 0;
	Queue queues[NUM_QUEUES];

	uint8_t cfg_select = 0;
	uint8_t cfg_subsel = 0;

	// The GUI thread appends, the CPU thread drains. `pending` is a
	// lock-free "is it worth taking the mutex" flag: pump() runs thousands
	// of times a second and is almost always looking at an empty queue.
	mutable std::mutex event_mutex;
	std::deque<Event> events;
	std::atomic<bool> pending{false};
};
