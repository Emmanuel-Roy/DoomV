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
// Not offered: 3D (virgl), blob resources, EDID. The cursor queue is taken
// and acknowledged; the pointer is drawn by the guest, as it is without the
// GPU.
#include "virtio_mmio.hpp"
#include <cstdint>
#include <map>
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

	VirtioGpu(uint32_t irq, uint32_t width, uint32_t height)
		: VirtioMmio(DEVICE_ID, VENDOR_DOOM, irq, 2), width(width), height(height) {}

	// A GPU is only there when the machine is given one (-gpu).
	void set_enabled(bool on) { enabled = on; }
	bool is_enabled() const { return enabled; }

private:
	struct Resource {
		uint32_t format = 0, w = 0, h = 0;
		std::vector<uint8_t> pixels;                          // host copy, w*h*4
		std::vector<std::pair<uint64_t, uint32_t>> backing;  // guest pages: address, length
	};

	bool present() const override { return enabled; }
	// struct virtio_gpu_config: events_read, events_clear, num_scanouts,
	// num_capsets. One scanout, no events, no 3D capability sets.
	uint8_t config_read8(uint64_t offset) const override { return offset == 8 ? 1 : 0; }
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

	const uint32_t width, height;   // the scanout: Memory::LFB_W x LFB_H
	bool enabled = false;
	std::map<uint32_t, Resource> resources;
	uint32_t scanout_resource = 0;   // 0: the scanout is off
	uint32_t scanout_x = 0, scanout_y = 0;   // where in that resource the screen starts
};
