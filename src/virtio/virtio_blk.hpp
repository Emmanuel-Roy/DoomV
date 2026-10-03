#pragma once
// virtio-blk over the MMIO transport.
//
// This is what a real root filesystem needs. Until now DoomV could boot
// Linux only from an initramfs -- the whole userland unpacked into RAM by
// the kernel before init runs -- which is fine for busybox and hopeless for
// a distribution: an Ubuntu rootfs is gigabytes, and it expects to *mount*
// a partitioned disk, fsck it, and write to it.
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
#include "virtio_mmio.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class VirtioBlk final : public VirtioMmio {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr uint32_t DEVICE_ID = 2;     // block device
	static constexpr uint64_t SECTOR    = 512;

	// The APLIC source this device drives. Each instance has its own -- an
	// MMIO virtio device has exactly one interrupt and two devices cannot
	// share it -- and the device tree has to name the same number for the
	// same address. The root disk is source 1.
	explicit VirtioBlk(uint32_t irq = 1) : VirtioMmio(DEVICE_ID, VENDOR_DOOM, irq, 1) {}
	~VirtioBlk() override;

	// Attach a backing image. Returns false if it cannot be opened, which
	// the caller reports rather than booting a machine whose disk silently
	// reads zeros.
	bool open(const std::string &path, bool read_only);
	void close();
	const std::string &path() const { return image_path; }
	bool attached() const { return file != nullptr; }
	bool read_only() const { return ro; }
	uint64_t capacity_sectors() const { return capacity / SECTOR; }

private:
	// No image, no device: id 0 is how a driver probing a fixed slot finds it
	// empty. Reporting a disk with no backing would mount one that reads zeros.
	bool present() const override { return attached(); }
	uint32_t features(uint32_t sel) const override;
	uint8_t config_read8(uint64_t offset) const override;
	void notify(unsigned q, Memory &mem, Aplic &aplic) override;

	bool do_io(Memory &mem, uint32_t type, uint64_t sector, uint64_t buf_addr, uint32_t buf_len);

	FILE *file = nullptr;
	std::string image_path;
	uint64_t capacity = 0;   // bytes
	bool ro = false;
	std::vector<uint8_t> io_buf;   // one request's data, between the file and guest RAM
};
