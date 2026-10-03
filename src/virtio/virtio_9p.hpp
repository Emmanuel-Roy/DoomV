#pragma once
// virtio-9p over the MMIO transport: a folder on the host that a Linux guest
// mounts directly.
//
// The storage drives in drives/ are disk images. Linux owns what is inside
// one, so the host can only look at it when the guest has let go of it --
// fine for storage, useless for moving a file between the two while both
// are running. A shared folder is the other half: the guest sees the host's
// files themselves, live, and a file saved on either side is there on the
// other immediately.
//
// This is how QEMU's virtfs does it. The device is a 9P file server: the
// guest's v9fs filesystem sends 9P2000.L requests down a virtqueue -- walk to
// a path, open it, read, write, list a directory -- and this answers each
// one against a real directory on the host. Linux has the client built in
// (CONFIG_9P_FS and CONFIG_NET_9P_VIRTIO), and the guest mounts it with
//
//   mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared
//
// where "shared" is the mount tag this device publishes in its config
// space.
//
// What it does not try to be: a POSIX filesystem. The host is Windows, which
// has no owners, modes, symlinks or case-sensitive names to offer, so every
// file belongs to root with permissive modes, the read-only attribute stands
// in for the write bit, and symlinks, device nodes and extended attributes
// are refused with EOPNOTSUPP rather than faked. See shared/README.md.
#include "virtio_mmio.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Virtio9p final : public VirtioMmio {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr uint32_t DEVICE_ID = 9;          // 9P transport
	// The APLIC source, matching the device tree node.
	static constexpr uint32_t IRQ = 12;

	Virtio9p() : VirtioMmio(DEVICE_ID, VENDOR_DOOM, IRQ, 1) {}

	// Serve `host_dir` under `mount_tag`. Returns false if the directory
	// cannot be used, in which case the slot stays empty (device ID 0).
	bool open(const std::string &host_dir, const std::string &mount_tag);
	bool attached() const { return is_open; }
	// The shared directory as an absolute host path, for messages.
	const std::string &host_root() const { return root_utf8; }

	~Virtio9p() override;

private:
	struct DirEntry {
		std::string name;
		bool dir;
	};

	// A 9P fid: the client's handle on a path, and on an open file once
	// it has opened one.
	struct Fid {
		std::string path;            // relative to the root, '/'-separated; "" is the root
		bool opened = false;
		void *handle = nullptr;      // host file handle, for an open regular file
		bool append = false;
		std::vector<DirEntry> listing;
		bool listed = false;
	};

	bool present() const override { return is_open; }
	// Word 0 bit 0 is VIRTIO_9P_MOUNT_TAG, without which the driver never
	// reads the tag and nothing can mount it; word 1 bit 0 is VERSION_1.
	uint32_t features(uint32_t) const override { return 1u; }
	uint8_t config_read8(uint64_t offset) const override;
	void notify(unsigned q, Memory &mem, Aplic &aplic) override;
	void reset() override { reset_fids(); }
	void handle_message(const std::vector<uint8_t> &req, std::vector<uint8_t> &resp);
	void close_fid(Fid &f);
	void reset_fids();

	bool is_open = false;
	std::string tag;
	std::string root_utf8;
	std::wstring root_w;       // extended-length form, for the host API
	std::wstring root_final;   // resolved and lower-cased, for the escape check

	std::map<uint32_t, Fid> fids;
	uint32_t msize = 8192;

	// Guest-visible metadata the host would otherwise decide, which would
	// make two runs of the same guest differ. See the comment above
	// NinePServer::guest_time in virtio_9p.cpp.
	uint64_t guest_ns = 0;                  // instruction count of the request being served
	std::map<uint64_t, uint64_t> ids;       // host file index -> inode number the guest sees
	uint64_t next_id = 1;
	uint64_t guest_id(uint64_t host_id)
	{
		const auto it = ids.find(host_id);
		if (it != ids.end()) return it->second;
		ids[host_id] = next_id;
		return next_id++;
	}
	void forget_id(uint64_t host_id) { ids.erase(host_id); }

	friend class NinePServer;
};
