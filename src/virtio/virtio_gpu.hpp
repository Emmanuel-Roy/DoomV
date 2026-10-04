#pragma once
// virtio-gpu over the MMIO transport, 2D only: a display controller with one
// scanout, the size of DoomV's window. Linux's virtio-gpu DRM driver runs it,
// and the console and X draw through DRM rather than into a fixed aperture.
//
// The model is the spec's: the guest creates resources -- images with a
// size and a pixel format -- and backs each with pages of its own RAM. It
// draws into those pages, then asks the device to TRANSFER a rectangle into
// the resource's host copy and to FLUSH a rectangle of the one on the
// scanout to the screen. Only a flush changes what is shown, so a frame is
// never displayed half drawn, and only what changed is copied.
//
// Every command is carried out inside the notify that posted it -- reading
// guest RAM, answering, raising the interrupt -- so the guest sees the same
// thing at the same instruction every run. What is shown is written into
// Memory's Linux framebuffer buffer, which the window, -fbdump and snapshots
// already read; with -gpu the device tree's simple-framebuffer node is
// removed, so nothing else draws there.
//
// With -gpu=virgl it is also a 3D GPU: VIRTIO_GPU_F_VIRGL, the guest's Mesa
// sends Gallium command streams, and virglrenderer (virgl_backend.cpp) runs
// them as OpenGL on the host GPU. Every command still completes before it is
// answered -- a fenced one waits for the host GPU -- but what the host GPU
// computes is outside the machine, like a network frame: so in that mode
// every response, and every byte a read-back writes into guest memory, is a
// result that -record logs and -replay hands back with no GPU at all.
//
// With -gpu=venus it is that and Vulkan too: Venus contexts (CONTEXT_INIT,
// capset 4), whose command rings live in shared memory the guest maps
// through blob resources (RESOURCE_BLOB) placed in a host-memory window in
// guest physical space. DoomV's build of virglrenderer runs Venus with no
// threads of its own (scripts/get_venus.py): rings are run, GPU work waited
// for and fences retired only inside step(), which runs at each notify and
// each input point -- so the host only ever writes that memory while the
// guest is stopped at an instruction that is the same every run.
//
// Not offered: EDID. The cursor queue is taken and acknowledged; the
// pointer is drawn by the guest, as it is without the GPU.
#include "virtio_mmio.hpp"
#include "virgl_backend.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>


class VirtioGpu final : public VirtioMmio {
	// Machine state is saved and restored field by field in savestate.cpp.
	friend struct SaveState;
public:
	static constexpr uint32_t DEVICE_ID = 16;   // GPU
	// Host copies of every resource together, at most: a guest that asks for
	// more is told it is out of memory, rather than the host running out.
	static constexpr uint64_t MEMORY_LIMIT = 512ull << 20;

	VirtioGpu(uint32_t irq, uint32_t width, uint32_t height);
	~VirtioGpu() override;

	// A GPU is only there when the machine is given one (-gpu).
	void set_enabled(bool on) { enabled = on; }
	bool is_enabled() const { return enabled; }
	// -gpu=virgl: 3D as well, through the host GPU.
	void set_virgl(bool on) { virgl = on; }
	bool is_virgl() const { return virgl; }
	// -gpu=venus: virgl and Venus (Vulkan) both.
	void set_venus(bool on) { venus = on; virgl = virgl || on; }
	bool is_venus() const { return venus; }

	// The host-memory window: where mapped blobs appear in guest physical
	// memory (VIRTIO_GPU_SHM_ID_HOST_VISIBLE), above any RAM -size -ram allows.
	static constexpr uint64_t HOSTMEM_BASE = 0x1000000000ull;   // 64GB
	static constexpr uint64_t HOSTMEM_SIZE = 0x100000000ull;    // 4GB
	// The bytes behind [offset, offset + len) of the window, or null where
	// nothing is mapped. A guest store goes through hostmem_write, so that the
	// step knows the guest has written and a recording knows what it wrote.
	uint8_t *hostmem(uint64_t offset, uint64_t len) const;
	bool hostmem_write(uint64_t offset, const void *data, uint64_t len);
	// Venus's step, at an input point: only when the guest has written the
	// shared memory since the last one, or every ALIVE_PERIOD instructions so
	// that the rings are reported alive. Inline, since the input points are
	// frequent and it is almost always off.
	static constexpr uint64_t ALIVE_PERIOD = 10'000'000;
	void tick(uint64_t now)
	{
		if (venus && (venus_dirty || now >= next_alive)) {
			next_alive = now + ALIVE_PERIOD;
			sync(SYNC_TICK);
		}
	}

	// What the host and the GPU write into Venus's shared memory is an input
	// too. At each sync point, while recording, every changed run of bytes
	// is handed to on_sync as (resource, offset, bytes); on a replay,
	// replay_sync supplies them and there is no GPU. A sync point is a tick
	// (SYNC_TICK) or a command that runs Venus (SYNC_COMMAND).
	enum SyncKind { SYNC_TICK = 0, SYNC_COMMAND = 1 };
	using BlobWrites = std::vector<std::tuple<uint32_t, uint64_t, std::vector<uint8_t>>>;
	std::function<void(SyncKind, const BlobWrites &)> on_sync;
	std::function<bool(SyncKind, BlobWrites &)> replay_sync;

	// In 3D mode, a command's result -- the response, and what it wrote
	// into guest memory, as (address, bytes) -- is an input. on_result sees
	// each one live; replay_result, when set, supplies them instead and the
	// host GPU is not used.
	using Writes = std::vector<std::pair<uint64_t, std::vector<uint8_t>>>;
	std::function<void(const std::vector<uint8_t> &, const Writes &)> on_result;
	std::function<bool(std::vector<uint8_t> &, Writes &)> replay_result;

	// In 3D mode the picture is the host GPU's, so it is not put anywhere the
	// guest could read -- a replay, with no GPU, has none, and the machine
	// must not differ for that. It goes here, for the window and -fbdump, in
	// the same x8r8g8b8 layout as the Linux framebuffer.
	const uint8_t *screen() const { return screen_buf.data(); }

	// Whether a program holds state on the host GPU that cannot be saved: a
	// virgl context that has been given commands, or anything of Venus's.
	// Otherwise -- the kernel's own context for the console, its buffers --
	// a snapshot saves the resources' contents and the contexts, and a
	// restore makes them again.
	bool has_3d_state() const
	{
		for (const auto &kv : virgl_contexts)
			if (kv.second.used) return true;
		return !venus_contexts.empty() || !venus_blobs.empty();
	}
	uint64_t screen_generation() const { return screen_gen.load(std::memory_order_relaxed); }

private:
	struct Resource {
		uint32_t format = 0, w = 0, h = 0;
		std::vector<uint8_t> pixels;                          // host copy, w*h*4
		std::vector<std::pair<uint64_t, uint32_t>> backing;  // guest pages: address, length
	};

	bool present() const override { return enabled; }
	// Word 0: bit 0 VIRTIO_GPU_F_VIRGL in 3D mode; with Venus, bit 3
	// RESOURCE_BLOB and bit 4 CONTEXT_INIT. Word 1: VERSION_1.
	uint32_t features(uint32_t sel) const override
	{
		if (sel == 1) return 1u;
		if (sel != 0) return 0;
		return (virgl ? 1u : 0u) | (venus ? (1u << 3) | (1u << 4) : 0u);
	}
	// struct virtio_gpu_config: events_read, events_clear, num_scanouts,
	// num_capsets. One scanout, no events; in 3D mode the two virgl
	// capability sets, and Venus's with it -- named whether or not the host
	// can supply them, so that what the guest negotiates does not depend on
	// the host.
	uint8_t config_read8(uint64_t offset) const override
	{
		return offset == 8 ? 1 : offset == 12 ? (venus ? 3 : virgl ? 2 : 0) : 0;
	}
	bool shm_region(uint32_t id, uint64_t &base, uint64_t &len) const override
	{
		if (!venus || id != 1) return false;   // VIRTIO_GPU_SHM_ID_HOST_VISIBLE
		base = HOSTMEM_BASE;
		len = HOSTMEM_SIZE;
		return true;
	}
	void notify(unsigned q, Memory &mem, Aplic &aplic) override;
	void reset() override;

	void control(Memory &mem, Aplic &aplic);
	void cursor(Memory &mem, Aplic &aplic);
	// Carries out one command; the response goes in `out`, header included.
	void command(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, Memory &mem);
	// Copy `len` bytes from `offset` within a resource's backing pages.
	bool read_backing(const Resource &r, uint64_t offset, uint8_t *dst, size_t len, Memory &mem) const;
	// Show the part of a rectangle of the scanout's resource that is on screen.
	void present(uint32_t x, uint32_t y, uint32_t w, uint32_t h, Memory &mem);
	void blank(Memory &mem);
	uint64_t resource_bytes() const;

	// 3D mode (virtio_gpu_virgl.cpp).
	void command_virgl(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, Writes &writes, Memory &mem);
	void present_virgl(Memory &mem);
	VirglBackend *host();   // started on first use, on the CPU thread
	struct Backing {
		std::vector<iovec> iov;        // host pointers into guest RAM, as virglrenderer keeps them
		std::vector<uint64_t> guest;   // the guest address of each
	};

	const uint32_t width, height;   // the scanout: Memory::LFB_W x LFB_H
	bool enabled = false;
	bool virgl = false;
	bool venus = false;
	// Venus: its contexts, its resources, and what is mapped where in the
	// host-memory window.
	std::vector<uint32_t> venus_contexts;
	struct Blob {
		uint32_t ctx = 0, map_info = 0;
		uint64_t size = 0;
		bool device = false;              // a VkDeviceMemory, which the GPU writes; else shared memory
		bool fresh = true;                // not yet compared since it was made
		uint8_t *bytes = nullptr;         // the host's memory, or `own` on a replay
		std::vector<uint8_t> own;         // a replay's stand-in for the host's memory
		std::vector<uint8_t> mirror;      // recording: the bytes as the guest last had them
	};
	std::map<uint32_t, Blob> venus_blobs;                            // by resource
	struct Mapping { uint64_t size; uint32_t resource; };
	std::map<uint64_t, Mapping> mappings;                            // by offset in the window
	bool venus_dirty = false;                                        // the guest wrote shared memory
	uint64_t next_alive = 0;
	bool is_venus_context(uint32_t ctx) const;
	// A sync point: live, run Venus and (recording) take down what changed;
	// on a replay, put back what was taken down.
	void sync(SyncKind kind);
	// The commands after which Venus runs.
	bool syncs_after(const std::vector<uint8_t> &in) const;
	// On a replay the commands are not run, but the device must still know
	// Venus's contexts and blobs, to map them and to know where the syncs are.
	void track_replayed(const std::vector<uint8_t> &in, const std::vector<uint8_t> &out);
	std::unique_ptr<VirglBackend> backend;
	bool backend_tried = false;
	std::map<uint32_t, Backing> backings;
	// Every virgl resource, with what it was made with and the guest pages
	// behind it: to free them on a reset, and to make them again after a
	// restore (restore_pending, done on the CPU thread at the first use).
	struct VirglRes {
		VirglResourceArgs args{};
		std::vector<std::pair<uint64_t, uint32_t>> backing;   // guest address, length
		std::vector<uint8_t> contents;                       // a snapshot's copy, level 0
	};
	std::map<uint32_t, VirglRes> virgl_res;
	struct VirglCtx {
		uint32_t init = 0;
		std::string name;
		std::vector<uint32_t> attached;   // resources
		bool used = false;                // has been given commands (SUBMIT_3D)
	};
	std::map<uint32_t, VirglCtx> virgl_contexts;
	bool restore_pending = false;
	void restore_resources(VirglBackend *v, Memory &mem);
	// For a snapshot: each resource's contents, read back from the host GPU
	// (or, on a replay, which has none, from the guest pages behind it).
	void save_contents(Memory &mem);
	// Bytes of a resource's level 0, as transfers lay it out with stride 0.
	static uint64_t level0_bytes(const VirglRes &r);
	static VirglResourceArgs create_args(const std::vector<uint8_t> &in);
	std::vector<uint8_t> readback;
	std::vector<uint8_t> screen_buf;
	std::atomic<uint64_t> screen_gen{0};
	std::map<uint32_t, Resource> resources;
	uint32_t scanout_resource = 0;   // 0: the scanout is off
	uint32_t scanout_x = 0, scanout_y = 0;   // where in that resource the screen starts
};
