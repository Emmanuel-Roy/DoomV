#pragma once
// The virtio MMIO transport (virtio 1.x, "version 2"), shared by every virtio
// device here: the register file, feature selection, the virtqueues and the
// interrupt. A device supplies only what is its own -- its id, its features,
// its config space, what a notify does and what a reset clears -- and the
// helpers below walk its rings.
//
// The MMIO transport is the simplest of virtio's three: no PCI enumeration.
// Each device sits at a fixed address the device tree names, and the driver
// finds it by reading a magic number. A device that is not there (an empty
// drive slot, no shared folder) answers device id 0, which the driver takes
// as "nothing here".
//
// A virtqueue is three rings at addresses the driver chooses:
//   descriptors  16 bytes each: addr u64, len u32, flags u16, next u16
//   available    flags u16, idx u16, ring[] u16 -- chains the driver offers
//   used         flags u16, idx u16, ring[] {id u32, len u32} -- chains done
#include <cstdint>

class Memory;
class Aplic;

class VirtioMmio {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
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
	static constexpr uint32_t MAGIC      = 0x74726976;   // "virt"
	static constexpr uint32_t VERSION    = 2;
	static constexpr uint32_t QUEUE_MAX  = 256;
	static constexpr unsigned MAX_QUEUES = 2;
	// Vendor ids: any value will do; these are the two in use.
	static constexpr uint32_t VENDOR_DOOM = 0x564d4f44;   // "DOMV"
	static constexpr uint32_t VENDOR_QEMU = 0x554d4551;   // "QEMU", the conventional value

	VirtioMmio(uint32_t device_id, uint32_t vendor, uint32_t irq, unsigned num_queues)
		: device_id(device_id), vendor(vendor), irq(irq), num_queues(num_queues) {}
	virtual ~VirtioMmio() = default;

	uint8_t  read8(uint64_t offset) const;
	uint32_t read32(uint64_t offset) const;
	void write8(uint64_t offset, uint8_t value);
	// A write can start I/O: a notify, so the guest's memory and the
	// interrupt controller come along.
	void write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic);

	uint32_t irq_source() const { return irq; }

protected:
	struct Queue {
		uint32_t num = 0;
		uint32_t ready = 0;
		uint64_t desc = 0, avail = 0, used = 0;
		// How far the device has consumed the available ring. The driver's
		// index only grows, so a notify processes what is new.
		uint16_t last_avail = 0;
	};
	struct Desc {
		uint64_t addr;
		uint32_t len;
		uint16_t flags, next;
	};
	static constexpr uint16_t DESC_F_NEXT  = 1;
	static constexpr uint16_t DESC_F_WRITE = 2;

	// ---- what a device decides ----------------------------------------------
	// Whether there is a device at all: false answers device id 0.
	virtual bool present() const { return true; }
	// The 32-bit word `sel` of the 64-bit feature set. Word 1 bit 0 is
	// VIRTIO_F_VERSION_1, which the version-2 transport needs.
	virtual uint32_t features(uint32_t sel) const { return sel == 1 ? 1u : 0u; }
	// Config space, from REG_CONFIG. Wider reads are built from bytes.
	virtual uint8_t config_read8(uint64_t) const { return 0; }
	virtual void config_write8(uint64_t, uint8_t) {}
	// The driver has posted buffers to queue `q`.
	virtual void notify(unsigned q, Memory &mem, Aplic &aplic) = 0;
	// Status 0: whatever the device holds beyond the transport, forgotten.
	virtual void reset() {}

	// ---- ring helpers ----------------------------------------------------------
	uint32_t queue_size(const Queue &q) const { return q.num ? q.num : QUEUE_MAX; }
	// The next chain the driver has offered on queue q, by its head
	// descriptor; false when there is none.
	bool next_chain(Memory &mem, unsigned q, uint16_t &head);
	// Whether queue q has a chain waiting, without taking it.
	bool has_chain(Memory &mem, unsigned q);
	Desc descriptor(Memory &mem, unsigned q, uint16_t index);
	// Return a chain to the driver, with the bytes the device wrote into it.
	void complete(Memory &mem, unsigned q, uint16_t head, uint32_t written);
	// Tell the driver the used ring moved.
	void interrupt(Aplic &aplic);

	const uint32_t device_id;
	const uint32_t vendor;
	const uint32_t irq;          // the APLIC source, matching the device tree node
	const unsigned num_queues;

	uint32_t status = 0;
	uint32_t device_feat_sel = 0;
	uint32_t driver_feat_sel = 0;
	uint32_t driver_feat[2] = {0, 0};
	uint32_t queue_sel = 0;
	uint32_t interrupt_status = 0;
	Queue queues[MAX_QUEUES];
};
