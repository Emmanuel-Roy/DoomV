#include "virtio_gpu.hpp"
#include "virgl_backend.hpp"
#include "memory.hpp"
#include <algorithm>
#include <cstring>

namespace {

// Command and response types, and the header (include/uapi/linux/virtio_gpu.h).
constexpr uint32_t CMD_GET_DISPLAY_INFO = 0x0100, CMD_RESOURCE_CREATE_2D = 0x0101,
                   CMD_RESOURCE_UNREF = 0x0102, CMD_SET_SCANOUT = 0x0103, CMD_RESOURCE_FLUSH = 0x0104,
                   CMD_TRANSFER_TO_HOST_2D = 0x0105, CMD_RESOURCE_ATTACH_BACKING = 0x0106,
                   CMD_RESOURCE_DETACH_BACKING = 0x0107;
constexpr uint32_t RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO = 0x1101, RESP_ERR_UNSPEC = 0x1200,
                   RESP_ERR_OUT_OF_MEMORY = 0x1201, RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
                   RESP_ERR_INVALID_RESOURCE_ID = 0x1203, RESP_ERR_INVALID_PARAMETER = 0x1205;
constexpr uint32_t FLAG_FENCE = 1;
constexpr size_t HDR = 24;          // type, flags, fence_id (8), ctx_id, ring_idx, padding
constexpr unsigned CONTROLQ = 0, CURSORQ = 1;
constexpr unsigned MAX_SCANOUTS = 16;

uint32_t le32(const std::vector<uint8_t> &b, size_t at)
{
	if (at + 4 > b.size()) return 0;
	return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
}

uint64_t le64(const std::vector<uint8_t> &b, size_t at)
{
	return le32(b, at) | (uint64_t)le32(b, at + 4) << 32;
}

void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
	if (b.size() < at + 4) b.resize(at + 4);
	for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (8 * i));
}

// Where each of the four bytes of a shown pixel -- blue, green, red, unused,
// the x8r8g8b8 the window displays -- comes from in a pixel of `format`.
// Null for a format this device does not take.
const uint8_t *swizzle(uint32_t format)
{
	static const uint8_t bgrx[4] = {0, 1, 2, 3}, xrgb[4] = {3, 2, 1, 0}, rgbx[4] = {2, 1, 0, 3},
	                     xbgr[4] = {1, 2, 3, 0};
	switch (format) {
	case 1: case 2:     return bgrx;   // B8G8R8A8, B8G8R8X8: the window's own layout
	case 3: case 4:     return xrgb;   // A8R8G8B8, X8R8G8B8
	case 67: case 134:  return rgbx;   // R8G8B8A8, R8G8B8X8
	case 68: case 121:  return xbgr;   // X8B8G8R8, A8B8G8R8
	default:            return nullptr;
	}
}

} // namespace

VirtioGpu::VirtioGpu(uint32_t irq, uint32_t width, uint32_t height)
	: VirtioMmio(DEVICE_ID, VENDOR_DOOM, irq, 2), width(width), height(height),
	  screen_buf((size_t)width * height * 4, 0) {}

VirtioGpu::~VirtioGpu() = default;

void VirtioGpu::notify(unsigned q, Memory &mem, Aplic &aplic)
{
	if (q == CONTROLQ) control(mem, aplic);
	else if (q == CURSORQ) cursor(mem, aplic);
}

void VirtioGpu::reset()
{
	if (backend) {
		for (uint32_t id : virgl_resources) {
			struct iovec *iov = nullptr;
			int n = 0;
			backend->resource_detach_iov((int)id, &iov, &n);
			backend->resource_unref(id);
		}
	}
	if (backend && venus) {
		for (const auto &[id, blob] : venus_blobs) backend->venus_destroy_resource(blob.first, id);
		for (uint32_t ctx : venus_contexts) backend->venus_destroy_context(ctx);
	}
	venus_blobs.clear();
	venus_contexts.clear();
	mappings.clear();
	virgl_resources.clear();
	backings.clear();
	resources.clear();
	scanout_resource = 0;
	scanout_x = scanout_y = 0;
}

uint64_t VirtioGpu::resource_bytes() const
{
	uint64_t total = 0;
	for (const auto &kv : resources) total += kv.second.pixels.size();
	return total;
}

void VirtioGpu::control(Memory &mem, Aplic &aplic)
{
	bool completed = false;
	uint16_t head;
	while (next_chain(mem, CONTROLQ, head)) {
		std::vector<uint8_t> in, out;
		std::vector<std::pair<uint64_t, uint32_t>> writable;
		uint16_t d = head;
		for (uint32_t guard = 0; guard <= queue_size(queues[CONTROLQ]); guard++) {
			const Desc desc = descriptor(mem, CONTROLQ, d);
			if (desc.flags & DESC_F_WRITE) {
				writable.push_back({desc.addr, desc.len});
			} else if (in.size() + desc.len <= (16u << 20)) {   // SUBMIT_3D carries command streams
				const size_t at = in.size();
				in.resize(at + desc.len);
				mem.read_bytes(desc.addr, in.data() + at, desc.len);
			}
			if (!(desc.flags & DESC_F_NEXT)) break;
			d = desc.next;
		}
		if (virgl) {
			Writes writes;
			if (replay_result) {
				if (!replay_result(out, writes)) {   // the log ran out: a replay of another run
					out.assign(HDR, 0);
					put32(out, 0, RESP_ERR_UNSPEC);
				}
			} else {
				command_virgl(in, out, writes, mem);
				if (on_result) on_result(out, writes);
			}
			for (const auto &[addr, bytes] : writes) mem.write_bytes(addr, bytes.data(), bytes.size());
		} else {
			command(in, out, mem);
		}
		uint32_t written = 0;
		for (const auto &[addr, len] : writable) {
			const uint32_t n = (uint32_t)std::min<size_t>(len, out.size() - written);
			if (n) mem.write_bytes(addr, out.data() + written, n);
			written += n;
		}
		complete(mem, CONTROLQ, head, written);
		completed = true;
	}
	if (completed) interrupt(aplic);
}

// The cursor queue's commands have no response. The pointer is the guest's
// to draw, so they are taken and returned.
void VirtioGpu::cursor(Memory &mem, Aplic &aplic)
{
	bool completed = false;
	uint16_t head;
	while (next_chain(mem, CURSORQ, head)) {
		complete(mem, CURSORQ, head, 0);
		completed = true;
	}
	if (completed) interrupt(aplic);
}

void VirtioGpu::command(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, Memory &mem)
{
	const uint32_t type = le32(in, 0);
	out.assign(HDR, 0);
	uint32_t resp = RESP_OK_NODATA;
	switch (type) {
	case CMD_GET_DISPLAY_INFO:
		// pmodes[16]: rect (x, y, width, height), enabled, flags. One on.
		resp = RESP_OK_DISPLAY_INFO;
		out.resize(HDR + MAX_SCANOUTS * 24, 0);
		put32(out, HDR + 8, width);
		put32(out, HDR + 12, height);
		put32(out, HDR + 16, 1);
		break;
	case CMD_RESOURCE_CREATE_2D: {
		const uint32_t id = le32(in, 24), format = le32(in, 28), w = le32(in, 32), h = le32(in, 36);
		const uint64_t bytes = (uint64_t)w * h * 4;
		if (id == 0 || resources.count(id)) resp = RESP_ERR_INVALID_RESOURCE_ID;
		else if (!swizzle(format) || w == 0 || h == 0 || w > 16384 || h > 16384) resp = RESP_ERR_INVALID_PARAMETER;
		else if (resource_bytes() + bytes > MEMORY_LIMIT) resp = RESP_ERR_OUT_OF_MEMORY;
		else {
			Resource &r = resources[id];
			r.format = format;
			r.w = w;
			r.h = h;
			r.pixels.assign(bytes, 0);
		}
		break;
	}
	case CMD_RESOURCE_UNREF: {
		const uint32_t id = le32(in, 24);
		if (!resources.erase(id)) resp = RESP_ERR_INVALID_RESOURCE_ID;
		else if (scanout_resource == id) {
			scanout_resource = 0;
			blank(mem);
		}
		break;
	}
	case CMD_SET_SCANOUT: {
		// rect, scanout_id, resource_id. The rect is the part of the
		// resource the screen shows; its size is the screen's.
		const uint32_t x = le32(in, 24), y = le32(in, 28), scanout = le32(in, 40), id = le32(in, 44);
		if (scanout != 0) { resp = RESP_ERR_INVALID_SCANOUT_ID; break; }
		if (id == 0) {
			scanout_resource = 0;
			blank(mem);
			break;
		}
		const auto it = resources.find(id);
		if (it == resources.end()) { resp = RESP_ERR_INVALID_RESOURCE_ID; break; }
		if (x >= it->second.w || y >= it->second.h) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		scanout_resource = id;
		scanout_x = x;
		scanout_y = y;
		blank(mem);
		present(0, 0, it->second.w, it->second.h, mem);
		break;
	}
	case CMD_RESOURCE_FLUSH: {
		const uint32_t x = le32(in, 24), y = le32(in, 28), w = le32(in, 32), h = le32(in, 36), id = le32(in, 40);
		if (!resources.count(id)) resp = RESP_ERR_INVALID_RESOURCE_ID;
		else if (id == scanout_resource) present(x, y, w, h, mem);
		break;
	}
	case CMD_TRANSFER_TO_HOST_2D: {
		// rect, offset (of the rectangle's first pixel in the backing), id.
		const uint32_t x = le32(in, 24), y = le32(in, 28), w = le32(in, 32), h = le32(in, 36);
		const uint64_t offset = le64(in, 40);
		const auto it = resources.find(le32(in, 48));
		if (it == resources.end()) { resp = RESP_ERR_INVALID_RESOURCE_ID; break; }
		Resource &r = it->second;
		if ((uint64_t)x + w > r.w || (uint64_t)y + h > r.h || r.backing.empty()) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		const uint64_t stride = (uint64_t)r.w * 4;
		for (uint32_t row = 0; row < h; row++) {
			uint8_t *dst = r.pixels.data() + (y + row) * stride + (uint64_t)x * 4;
			if (!read_backing(r, offset + row * stride, dst, (size_t)w * 4, mem)) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		}
		break;
	}
	case CMD_RESOURCE_ATTACH_BACKING: {
		// id, nr_entries, then that many { addr u64, length u32, padding }.
		const auto it = resources.find(le32(in, 24));
		const uint32_t n = le32(in, 28);
		if (it == resources.end()) { resp = RESP_ERR_INVALID_RESOURCE_ID; break; }
		if (32 + (uint64_t)n * 16 > in.size()) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		it->second.backing.clear();
		for (uint32_t i = 0; i < n; i++)
			it->second.backing.push_back({le64(in, 32 + i * 16), le32(in, 40 + i * 16)});
		break;
	}
	case CMD_RESOURCE_DETACH_BACKING: {
		const auto it = resources.find(le32(in, 24));
		if (it == resources.end()) resp = RESP_ERR_INVALID_RESOURCE_ID;
		else it->second.backing.clear();
		break;
	}
	default:
		resp = RESP_ERR_UNSPEC;   // 3D, blobs, EDID, capsets: not offered
		break;
	}
	put32(out, 0, resp);
	// A fenced command is answered with its fence: done, since every command
	// is done by the time it is answered.
	const uint32_t flags = le32(in, 4);
	if (flags & FLAG_FENCE) {
		put32(out, 4, FLAG_FENCE);
		std::memcpy(out.data() + 8, in.data() + 8, 8);    // fence_id
		std::memcpy(out.data() + 16, in.data() + 16, 4);  // ctx_id
	}
}

bool VirtioGpu::read_backing(const Resource &r, uint64_t offset, uint8_t *dst, size_t len, Memory &mem) const
{
	uint64_t at = 0;   // where the current entry starts in the backing
	for (const auto &[addr, elen] : r.backing) {
		if (len == 0) break;
		if (offset < at + elen) {
			const uint64_t skip = offset - at;
			const size_t n = (size_t)std::min<uint64_t>(len, elen - skip);
			mem.read_bytes(addr + skip, dst, n);
			dst += n;
			len -= n;
			offset += n;
		}
		at += elen;
	}
	return len == 0;
}

void VirtioGpu::present(uint32_t x, uint32_t y, uint32_t w, uint32_t h, Memory &mem)
{
	const auto it = resources.find(scanout_resource);
	if (it == resources.end()) return;
	const Resource &r = it->second;
	const uint8_t *map = swizzle(r.format);
	// The rectangle, in the resource, clipped to the resource and to the part
	// of it on screen.
	const uint64_t x0 = std::max<uint64_t>(x, scanout_x), y0 = std::max<uint64_t>(y, scanout_y);
	const uint64_t x1 = std::min<uint64_t>({(uint64_t)x + w, r.w, (uint64_t)scanout_x + width});
	const uint64_t y1 = std::min<uint64_t>({(uint64_t)y + h, r.h, (uint64_t)scanout_y + height});
	if (x0 >= x1 || y0 >= y1 || !map) return;
	uint8_t *screen = mem.linux_framebuffer_mut();
	for (uint64_t row = y0; row < y1; row++) {
		const uint8_t *src = r.pixels.data() + (row * r.w + x0) * 4;
		uint8_t *dst = screen + ((row - scanout_y) * width + (x0 - scanout_x)) * 4;
		if (map[0] == 0 && map[1] == 1 && map[2] == 2) {
			std::memcpy(dst, src, (x1 - x0) * 4);
		} else {
			for (uint64_t i = 0; i < x1 - x0; i++, src += 4, dst += 4) {
				dst[0] = src[map[0]]; dst[1] = src[map[1]]; dst[2] = src[map[2]]; dst[3] = 0;
			}
		}
	}
	mem.linux_framebuffer_touched();
}

void VirtioGpu::blank(Memory &mem)
{
	if (virgl) {
		std::memset(screen_buf.data(), 0, screen_buf.size());
		screen_gen.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	std::memset(mem.linux_framebuffer_mut(), 0, (size_t)width * height * 4);
	mem.linux_framebuffer_touched();
}
