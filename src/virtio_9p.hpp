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
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Memory;
class Aplic;

class Virtio9p {
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

	static constexpr uint32_t MAGIC     = 0x74726976; // "virt"
	static constexpr uint32_t VERSION   = 2;
	static constexpr uint32_t DEVICE_ID = 9;          // 9P transport
	static constexpr uint32_t QUEUE_MAX = 256;
	// The APLIC source, matching the device tree node.
	static constexpr uint32_t IRQ = 12;

	// Serve `host_dir` under `mount_tag`. Returns false if the directory
	// cannot be used, in which case the slot stays empty (device ID 0).
	bool open(const std::string &host_dir, const std::string &mount_tag);
	bool attached() const { return is_open; }
	// The shared directory as an absolute host path, for messages.
	const std::string &host_root() const { return root_utf8; }

	uint8_t  read8(uint64_t offset) const;
	uint32_t read32(uint64_t offset) const;
	void write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic);

	~Virtio9p();

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

	void process_queue(Memory &mem, Aplic &aplic);
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

	uint32_t status = 0;
	uint32_t device_feat_sel = 0;
	uint32_t queue_sel = 0;
	uint32_t queue_num = 0;
	uint32_t queue_ready = 0;
	uint32_t interrupt_status = 0;
	uint64_t desc_addr = 0, avail_addr = 0, used_addr = 0;
	uint16_t last_avail = 0;

	friend class NinePServer;
};
