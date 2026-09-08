#pragma once
// virtio-blk over the MMIO transport.
//
// This is what a real root filesystem needs. Until now DoomV could boot
// Linux only from an initramfs -- the whole userland unpacked into RAM by
// the kernel before init runs -- which is fine for busybox and hopeless for
// a distribution: an Ubuntu rootfs is gigabytes, and it expects to *mount*
// a partitioned disk, fsck it, and write to it.
//
// The MMIO transport is the simplest of virtio's three. There is no PCI
// enumeration: the device sits at a fixed address, the device tree says so,
// and the driver finds it by reading a magic number. Version 2 ("modern")
// is what Linux prefers and is less work than legacy, because the queue
// addresses are written as plain 64-bit physical addresses rather than
// page-frame numbers.
//
// A request is a descriptor chain of three parts, and the split matters
// because the driver chooses where each part lives:
//
//   header   16 bytes: type, a reserved word, and the 512-byte sector
//   data     one or more descriptors, the actual payload
//   status   one byte the device writes to say how it went
//
// The device walks the available ring for indices the driver has published,
// follows each chain, performs the I/O against the backing file, writes the
// status byte, and publishes the descriptor index in the used ring. Then it
// raises its interrupt line.
#include <cstdint>
#include <cstdio>
#include <string>

class Memory;
class Aplic;

class VirtioBlk {
public:
	// Register offsets, from the virtio 1.x MMIO transport. Named rather
	// than commented individually -- the spec's own names are the clearest
	// documentation there is for these.
	enum : uint64_t {
		REG_MAGIC          = 0x000,
		REG_VERSION        = 0x004,
		REG_DEVICE_ID      = 0x008,
		REG_VENDOR_ID      = 0x00c,
		REG_DEVICE_FEAT    = 0x010,
		REG_DEVICE_FEAT_SEL= 0x014,
		REG_DRIVER_FEAT    = 0x020,
		REG_DRIVER_FEAT_SEL= 0x024,
		REG_QUEUE_SEL      = 0x030,
		REG_QUEUE_NUM_MAX  = 0x034,
		REG_QUEUE_NUM      = 0x038,
		REG_QUEUE_READY    = 0x044,
		REG_QUEUE_NOTIFY   = 0x050,
		REG_INTERRUPT_STAT = 0x060,
		REG_INTERRUPT_ACK  = 0x064,
		REG_STATUS         = 0x070,
		REG_QUEUE_DESC_LO  = 0x080,
		REG_QUEUE_DESC_HI  = 0x084,
		REG_QUEUE_AVAIL_LO = 0x090,
		REG_QUEUE_AVAIL_HI = 0x094,
		REG_QUEUE_USED_LO  = 0x0a0,
		REG_QUEUE_USED_HI  = 0x0a4,
		REG_CONFIG_GEN     = 0x0fc,
		REG_CONFIG         = 0x100,
	};

	static constexpr uint32_t MAGIC     = 0x74726976; // "virt"
	static constexpr uint32_t VERSION   = 2;
	static constexpr uint32_t DEVICE_ID = 2;          // block device
	static constexpr uint32_t QUEUE_MAX = 256;
	static constexpr uint64_t SECTOR    = 512;

	// Attach a backing image. Returns false if it cannot be opened, which
	// the caller reports rather than booting a machine whose disk silently
	// reads zeros.
	bool open(const std::string &path, bool read_only);
	bool attached() const { return file != nullptr; }
	uint64_t capacity_sectors() const { return capacity / SECTOR; }

	uint32_t read32(uint64_t offset) const;
	// Writes can start I/O, so this needs the guest's memory to read
	// descriptors from and write payloads into, and the interrupt
	// controller to signal completion through.
	void write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic);

	// The APLIC source this device drives. Fixed rather than configurable
	// because the device tree has to name the same number.
	static constexpr uint32_t IRQ = 1;

	~VirtioBlk();

private:
	void process_queue(Memory &mem, Aplic &aplic);
	bool do_io(Memory &mem, uint32_t type, uint64_t sector,
	           uint64_t buf_addr, uint32_t buf_len);

	FILE *file = nullptr;
	uint64_t capacity = 0;   // bytes
	bool ro = false;

	uint32_t status = 0;
	uint32_t device_feat_sel = 0;
	uint32_t driver_feat_sel = 0;
	uint32_t driver_feat[2] = {0, 0};
	uint32_t queue_num = 0;
	uint32_t queue_ready = 0;
	uint32_t interrupt_status = 0;
	uint64_t desc_addr = 0, avail_addr = 0, used_addr = 0;
	// The driver publishes an ever-increasing index; the device remembers
	// how far it has consumed so a notify only processes what is new.
	uint16_t last_avail = 0;
};
