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
// Not offered: blob resources, EDID. The cursor queue is taken and
// acknowledged; the pointer is drawn by the guest, as it is without the GPU.
#include "virtio_mmio.hpp"
#include "virgl_backend.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
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
	uint64_t screen_generation() const { return screen_gen.load(std::memory_order_relaxed); }

private:
	struct Resource {
		uint32_t format = 0, w = 0, h = 0;
		std::vector<uint8_t> pixels;                          // host copy, w*h*4
		std::vector<std::pair<uint64_t, uint32_t>> backing;  // guest pages: address, length
	};

	bool present() const override { return enabled; }
	// Word 0 bit 0: VIRTIO_GPU_F_VIRGL, in 3D mode. Word 1: VERSION_1.
	uint32_t features(uint32_t sel) const override { return sel == 0 ? (virgl ? 1u : 0u) : sel == 1 ? 1u : 0u; }
	// struct virtio_gpu_config: events_read, events_clear, num_scanouts,
	// num_capsets. One scanout, no events; in 3D mode the two virgl
	// capability sets -- named whether or not the host can supply them, so
	// that what the guest negotiates does not depend on the host.
	uint8_t config_read8(uint64_t offset) const override
	{
		return offset == 8 ? 1 : offset == 12 ? (virgl ? 2 : 0) : 0;
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
	std::unique_ptr<VirglBackend> backend;
	bool backend_tried = false;
	std::map<uint32_t, Backing> backings;
	std::vector<uint32_t> virgl_resources;   // every resource the host has, for a reset
	std::vector<uint8_t> readback;
	std::vector<uint8_t> screen_buf;
	std::atomic<uint64_t> screen_gen{0};
	std::map<uint32_t, Resource> resources;
	uint32_t scanout_resource = 0;   // 0: the scanout is off
	uint32_t scanout_x = 0, scanout_y = 0;   // where in that resource the screen starts
};
