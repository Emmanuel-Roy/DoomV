#include "virtio_blk.hpp"
#include "memory.hpp"
#include "aplic.hpp"
#include <cstring>

namespace {

// Split-virtqueue layout, from the virtio 1.x spec. The three rings sit at
// addresses the driver supplies independently, so nothing here assumes they
// are adjacent -- which they are not, once the driver allocates them
// separately.
constexpr uint64_t DESC_SIZE = 16;      // addr u64, len u32, flags u16, next u16
constexpr uint16_t DESC_F_NEXT = 1;

// Request types, from virtio-blk.
constexpr uint32_t BLK_T_IN     = 0;    // read from disk into memory
constexpr uint32_t BLK_T_OUT    = 1;    // write memory to disk
constexpr uint32_t BLK_T_FLUSH  = 4;
constexpr uint32_t BLK_T_GET_ID = 8;

constexpr uint8_t BLK_S_OK     = 0;
constexpr uint8_t BLK_S_IOERR  = 1;
constexpr uint8_t BLK_S_UNSUPP = 2;

// The one feature bit that matters for correctness here. VIRTIO_F_VERSION_1
// is bit 32 of the 64-bit feature set -- bit 0 of the high word -- and it is
// what tells the driver this is a modern device. Without it Linux falls back
// to legacy layout rules the register interface here does not implement.
constexpr uint32_t FEAT_HI_VERSION_1 = 1u << 0;

// 64-bit file positioning, because `long` is 32 bits on this toolchain and a
// disk image is the one file here that outgrows it. Both of the calls these
// replace failed silently and differently:
//
//   ftell  on a 4GiB image returns -1 (the size is exactly 2^32), so the
//          capacity came out zero, the guest saw "[vda] 0 512-byte logical
//          blocks", and the kernel panicked with "Unable to mount root fs on
//          unknown-block(254,1)" -- a message about the *partition* that says
//          nothing about the device having no size.
//
//   fseek  with the offset cast to long wrapped every access past 2GiB back
//          into the low half of the image. That one is worse: it does not
//          fail, it reads and writes the wrong sectors, so a filesystem
//          larger than 2GiB would corrupt itself quietly.
//
// MinGW spells these _fseeki64/_ftelli64; POSIX has fseeko/ftello with off_t
// 64 bits wide once _FILE_OFFSET_BITS says so.
#if defined(_WIN32)
static inline int seek64(std::FILE *f, int64_t off, int whence) { return _fseeki64(f, off, whence); }
static inline int64_t tell64(std::FILE *f) { return _ftelli64(f); }
#else
static inline int seek64(std::FILE *f, int64_t off, int whence) { return fseeko(f, (off_t)off, whence); }
static inline int64_t tell64(std::FILE *f) { return (int64_t)ftello(f); }
#endif

} // namespace

VirtioBlk::~VirtioBlk()
{
	if (file) std::fclose(file);
}

bool VirtioBlk::open(const std::string &path, bool read_only)
{
	// Opened read-write unless asked otherwise: a distribution rootfs is
	// mounted writable, replays its journal on first boot, and fails in
	// confusing ways if writes appear to succeed without landing. Falling
	// back to read-only beats refusing to boot, and the caller is told
	// which it got so it can say so.
	file = std::fopen(path.c_str(), read_only ? "rb" : "r+b");
	if (!file) {
		file = std::fopen(path.c_str(), "rb");
		read_only = true;
	}
	if (!file) return false;
	ro = read_only;
	if (seek64(file, 0, SEEK_END) != 0) return false;
	const int64_t end = tell64(file);
	capacity = end > 0 ? (uint64_t)end : 0;
	seek64(file, 0, SEEK_SET);
	return true;
}

uint32_t VirtioBlk::read32(uint64_t offset) const
{
	switch (offset) {
	case REG_MAGIC:     return MAGIC;
	case REG_VERSION:   return VERSION;
	// Device ID 0 means "nothing at this address", which is how a driver
	// probing a fixed MMIO slot discovers the slot is empty. Reporting the
	// block ID with no backing file would have Linux mount a disk whose
	// every sector reads as zero.
	case REG_DEVICE_ID: return attached() ? DEVICE_ID : 0;
	case REG_VENDOR_ID: return 0x564d4f44;
	case REG_DEVICE_FEAT:
		// The feature set is 64 bits, read one word at a time through the
		// selector. Only VERSION_1 is offered: read-only, discard, write
		// zeroes and the rest are optimisations this device does not
		// implement, and claiming one it will not honour is worse than
		// claiming none at all.
		return device_feat_sel == 1 ? FEAT_HI_VERSION_1 : 0;
	case REG_QUEUE_NUM_MAX:  return QUEUE_MAX;
	case REG_QUEUE_READY:    return queue_ready;
	case REG_INTERRUPT_STAT: return interrupt_status;
	case REG_STATUS:         return status;
	case REG_CONFIG_GEN:     return 0;
	// virtio_blk_config begins with the capacity in 512-byte sectors, a
	// 64-bit little-endian value the driver reads as two words.
	case REG_CONFIG:         return (uint32_t)(capacity_sectors() & 0xFFFFFFFFull);
	case REG_CONFIG + 4:     return (uint32_t)(capacity_sectors() >> 32);
	default: return 0;
	}
}

void VirtioBlk::write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic)
{
	switch (offset) {
	case REG_DEVICE_FEAT_SEL: device_feat_sel = value; return;
	case REG_DRIVER_FEAT_SEL: driver_feat_sel = value; return;
	case REG_DRIVER_FEAT:
		if (driver_feat_sel < 2) driver_feat[driver_feat_sel] = value;
		return;
	case REG_QUEUE_SEL:   return;   // one queue; selecting another is ignored
	case REG_QUEUE_NUM:   queue_num = value; return;
	case REG_QUEUE_READY: queue_ready = value; return;
	case REG_QUEUE_DESC_LO:  desc_addr  = (desc_addr  & ~0xFFFFFFFFull) | value; return;
	case REG_QUEUE_DESC_HI:  desc_addr  = (desc_addr  & 0xFFFFFFFFull) | ((uint64_t)value << 32); return;
	case REG_QUEUE_AVAIL_LO: avail_addr = (avail_addr & ~0xFFFFFFFFull) | value; return;
	case REG_QUEUE_AVAIL_HI: avail_addr = (avail_addr & 0xFFFFFFFFull) | ((uint64_t)value << 32); return;
	case REG_QUEUE_USED_LO:  used_addr  = (used_addr  & ~0xFFFFFFFFull) | value; return;
	case REG_QUEUE_USED_HI:  used_addr  = (used_addr  & 0xFFFFFFFFull) | ((uint64_t)value << 32); return;
	case REG_STATUS:
		status = value;
		// Writing zero is a device reset, and the queue state has to go
		// with it -- including how far the device had consumed the
		// available ring. Carrying that across a reset makes the next
		// boot skip its first request.
		if (value == 0) {
			queue_ready = 0;
			last_avail = 0;
			interrupt_status = 0;
			desc_addr = avail_addr = used_addr = 0;
		}
		return;
	case REG_INTERRUPT_ACK:
		interrupt_status &= ~value;
		return;
	case REG_QUEUE_NOTIFY:
		if (queue_ready && attached()) process_queue(mem, aplic);
		return;
	default: return;
	}
}

bool VirtioBlk::do_io(Memory &mem, uint32_t type, uint64_t sector,
                      uint64_t buf_addr, uint32_t buf_len)
{
	const uint64_t off = sector * SECTOR;
	if (off + buf_len > capacity) return false;
	if (seek64(file, (int64_t)off, SEEK_SET) != 0) return false;

	// The payload moves a byte at a time through Memory rather than by bulk
	// pointer, because the guest buffer is a *physical* address that need
	// not be plain RAM -- and Memory is the only thing that knows which
	// addresses are backed and which are devices.
	if (type == BLK_T_IN) {
		for (uint32_t i = 0; i < buf_len; i++) {
			int c = std::fgetc(file);
			mem.write8(buf_addr + i, (uint8_t)(c < 0 ? 0 : c));
		}
		return true;
	}
	if (ro) return false;
	for (uint32_t i = 0; i < buf_len; i++)
		std::fputc(mem.read8(buf_addr + i), file);
	std::fflush(file);
	return true;
}

// Memory exposes write8/write32/write64 but no write16, so the used-ring
// index -- the one genuinely 16-bit field a device has to publish -- is
// written as two bytes. Little-endian, to match every other access here.
static void write16_phys(Memory &mem, uint64_t addr, uint16_t v)
{
	mem.write8(addr, (uint8_t)(v & 0xFF));
	mem.write8(addr + 1, (uint8_t)(v >> 8));
}

void VirtioBlk::process_queue(Memory &mem, Aplic &aplic)
{
	// avail ring: flags u16, idx u16, ring[] u16
	const uint16_t avail_idx = mem.read16(avail_addr + 2);
	const uint32_t qsz = queue_num ? queue_num : QUEUE_MAX;
	bool completed = false;

	while (last_avail != avail_idx) {
		const uint16_t slot = (uint16_t)(last_avail % qsz);
		const uint16_t head = mem.read16(avail_addr + 4 + (uint64_t)slot * 2);

		// Walk the chain. The header comes first and the status byte last,
		// with payload in between -- but the driver may split that payload
		// across any number of descriptors, so this accumulates rather
		// than assuming a chain of exactly three.
		uint32_t type = 0;
		uint64_t sector = 0;
		uint64_t status_addr = 0;
		uint32_t total = 0;
		bool ok = true;
		bool first = true;

		uint16_t d = head;
		for (uint32_t guard = 0; guard <= qsz; guard++) {
			const uint64_t da    = desc_addr + (uint64_t)d * DESC_SIZE;
			const uint64_t addr  = mem.read64(da);
			const uint32_t len   = mem.read32(da + 8);
			const uint16_t flags = mem.read16(da + 12);
			const uint16_t next  = mem.read16(da + 14);

			if (first) {
				type   = mem.read32(addr);
				sector = mem.read64(addr + 8);
				first  = false;
			} else if (!(flags & DESC_F_NEXT) && len == 1) {
				// A one-byte descriptor ending the chain is where the
				// device reports its status.
				status_addr = addr;
			} else {
				if (type == BLK_T_IN || type == BLK_T_OUT) {
					if (!do_io(mem, type, sector + total / SECTOR, addr, len))
						ok = false;
					total += len;
				} else if (type == BLK_T_GET_ID) {
					// A device identity string. Anything printable will
					// do; it is only ever shown to people.
					static const char id[] = "doomv-virtio-blk";
					for (uint32_t i = 0; i < len; i++)
						mem.write8(addr + i, i < sizeof(id) ? (uint8_t)id[i] : 0);
				}
			}

			if (!(flags & DESC_F_NEXT)) break;
			d = next;
		}

		uint8_t st = BLK_S_OK;
		if (type != BLK_T_IN && type != BLK_T_OUT && type != BLK_T_FLUSH
		    && type != BLK_T_GET_ID)
			st = BLK_S_UNSUPP;
		else if (!ok)
			st = BLK_S_IOERR;
		if (status_addr) mem.write8(status_addr, st);

		// used ring: flags u16, idx u16, ring[]{id u32, len u32}
		const uint16_t used_idx  = mem.read16(used_addr + 2);
		const uint64_t slot_addr = used_addr + 4 + (uint64_t)(used_idx % qsz) * 8;
		mem.write32(slot_addr, head);
		mem.write32(slot_addr + 4, total);
		// The index is published only after the entry it refers to, so a
		// driver that observes the new index always sees a complete entry.
		write16_phys(mem, used_addr + 2, (uint16_t)(used_idx + 1));

		last_avail++;
		completed = true;
	}

	if (completed) {
		interrupt_status |= 1;   // the used ring was updated
		aplic.assert_source(IRQ);
	}
}
