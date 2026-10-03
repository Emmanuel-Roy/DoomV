#include "virtio_blk.hpp"
#include "memory.hpp"
#include "aplic.hpp"
#include <cstring>

namespace {

// Request types, from virtio-blk.
constexpr uint32_t BLK_T_IN     = 0;    // read from disk into memory
constexpr uint32_t BLK_T_OUT    = 1;    // write memory to disk
constexpr uint32_t BLK_T_FLUSH  = 4;
constexpr uint32_t BLK_T_GET_ID = 8;

constexpr uint8_t BLK_S_OK     = 0;
constexpr uint8_t BLK_S_IOERR  = 1;
constexpr uint8_t BLK_S_UNSUPP = 2;

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
	close();
	file = std::fopen(path.c_str(), read_only ? "rb" : "r+b");
	if (!file) {
		file = std::fopen(path.c_str(), "rb");
		read_only = true;
	}
	if (!file) return false;
	ro = read_only;
	image_path = path;
	if (seek64(file, 0, SEEK_END) != 0) return false;
	const int64_t end = tell64(file);
	capacity = end > 0 ? (uint64_t)end : 0;
	seek64(file, 0, SEEK_SET);
	return true;
}

void VirtioBlk::close()
{
	if (file) std::fclose(file);
	file = nullptr;
	image_path.clear();
}

// VIRTIO_F_VERSION_1 in the high word, and in the low word only
// VIRTIO_BLK_F_RO (bit 5), when the image could only be opened read-only:
// then Linux marks the disk read-only and a mount falls back to ro, where
// without the bit every write would fail long after the mount appeared to
// succeed. Discard, write-zeroes and the rest are optimisations this device
// does not do, and claiming one it will not honour is worse than none.
uint32_t VirtioBlk::features(uint32_t sel) const
{
	if (sel == 1) return 1u;
	return sel == 0 && ro ? (1u << 5) : 0;
}

// virtio_blk_config begins with the capacity in 512-byte sectors, a 64-bit
// little-endian value the driver reads as two words.
uint8_t VirtioBlk::config_read8(uint64_t offset) const
{
	return offset < 8 ? (uint8_t)(capacity_sectors() >> (8 * offset)) : 0;
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
	// Whole buffers, still inside the notify that asked for them: the guest
	// sees the data and the completion at exactly the instruction it always
	// did. What changed is the cost -- this was an fgetc or fputc and a
	// guest-memory call per byte, 8192 calls for one 4KiB block. A short
	// read past the end of the file reads as zeros, as fgetc's EOF did.
	io_buf.resize(buf_len);
	if (type == BLK_T_IN) {
		const size_t got = std::fread(io_buf.data(), 1, buf_len, file);
		if (got < buf_len) std::memset(io_buf.data() + got, 0, buf_len - got);
		mem.write_bytes(buf_addr, io_buf.data(), buf_len);
		return true;
	}
	if (ro) return false;
	mem.read_bytes(buf_addr, io_buf.data(), buf_len);
	std::fwrite(io_buf.data(), 1, buf_len, file);
	// Flushed per request, as before. The guest is never told to flush --
	// VIRTIO_BLK_F_FLUSH is not offered -- so this is the only point at which
	// its writes reach the operating system, and a run that is killed keeps
	// everything up to here.
	std::fflush(file);
	return true;
}

// A request is a chain: a 16-byte header (type, reserved, sector), the data
// in any number of descriptors, and a one-byte status at the end.
void VirtioBlk::notify(unsigned, Memory &mem, Aplic &aplic)
{
	bool completed = false;
	uint16_t head;
	while (next_chain(mem, 0, head)) {
		uint32_t type = 0;
		uint64_t sector = 0;
		uint64_t status_addr = 0;
		uint32_t total = 0;
		bool ok = true;
		bool first = true;

		uint16_t d = head;
		for (uint32_t guard = 0; guard <= queue_size(queues[0]); guard++) {
			const Desc desc = descriptor(mem, 0, d);
			if (first) {
				type   = mem.read32(desc.addr);
				sector = mem.read64(desc.addr + 8);
				first  = false;
			} else if (!(desc.flags & DESC_F_NEXT) && desc.len == 1) {
				status_addr = desc.addr;   // the one-byte status ends the chain
			} else if (type == BLK_T_IN || type == BLK_T_OUT) {
				if (!do_io(mem, type, sector + total / SECTOR, desc.addr, desc.len)) ok = false;
				total += desc.len;
			} else if (type == BLK_T_GET_ID) {
				// A device identity string, only ever shown to people.
				static const char id[] = "doomv-virtio-blk";
				for (uint32_t i = 0; i < desc.len; i++)
					mem.write8(desc.addr + i, i < sizeof(id) ? (uint8_t)id[i] : 0);
			}
			if (!(desc.flags & DESC_F_NEXT)) break;
			d = desc.next;
		}

		uint8_t st = BLK_S_OK;
		if (type != BLK_T_IN && type != BLK_T_OUT && type != BLK_T_FLUSH && type != BLK_T_GET_ID)
			st = BLK_S_UNSUPP;
		else if (!ok)
			st = BLK_S_IOERR;
		if (status_addr) mem.write8(status_addr, st);

		complete(mem, 0, head, total);
		completed = true;
	}
	if (completed) interrupt(aplic);
}
