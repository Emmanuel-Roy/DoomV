// virtio-gpu's 3D mode: each command handed to virglrenderer, which runs the
// guest's OpenGL on the host GPU. See virtio_gpu.hpp for what is recorded and
// why; this file only carries the commands out.
#include "virtio_gpu.hpp"
#include "virgl_backend.hpp"
#include "memory.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace {

constexpr uint32_t CMD_GET_DISPLAY_INFO = 0x0100, CMD_RESOURCE_CREATE_2D = 0x0101, CMD_RESOURCE_UNREF = 0x0102,
                   CMD_SET_SCANOUT = 0x0103, CMD_RESOURCE_FLUSH = 0x0104, CMD_TRANSFER_TO_HOST_2D = 0x0105,
                   CMD_RESOURCE_ATTACH_BACKING = 0x0106, CMD_RESOURCE_DETACH_BACKING = 0x0107,
                   CMD_GET_CAPSET_INFO = 0x0108, CMD_GET_CAPSET = 0x0109,
                   CMD_CTX_CREATE = 0x0200, CMD_CTX_DESTROY = 0x0201, CMD_CTX_ATTACH_RESOURCE = 0x0202,
                   CMD_CTX_DETACH_RESOURCE = 0x0203, CMD_RESOURCE_CREATE_3D = 0x0204,
                   CMD_TRANSFER_TO_HOST_3D = 0x0205, CMD_TRANSFER_FROM_HOST_3D = 0x0206, CMD_SUBMIT_3D = 0x0207,
                   CMD_RESOURCE_CREATE_BLOB = 0x010c, CMD_RESOURCE_MAP_BLOB = 0x0208,
                   CMD_RESOURCE_UNMAP_BLOB = 0x0209;
constexpr uint32_t RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO = 0x1101, RESP_OK_CAPSET_INFO = 0x1102,
                   RESP_OK_CAPSET = 0x1103, RESP_OK_MAP_INFO = 0x1106, RESP_ERR_UNSPEC = 0x1200,
                   RESP_ERR_OUT_OF_MEMORY = 0x1201, RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
                   RESP_ERR_INVALID_RESOURCE_ID = 0x1203, RESP_ERR_INVALID_PARAMETER = 0x1205;
constexpr uint32_t FLAG_FENCE = 1, FLAG_INFO_RING_IDX = 2;
constexpr uint32_t CAPSET_VENUS = 4;
constexpr uint32_t BLOB_MEM_HOST3D = 2;
constexpr size_t HDR = 24;
constexpr unsigned MAX_SCANOUTS = 16;
constexpr uint32_t CAPSETS[] = {1, 2, 4};       // VIRTIO_GPU_CAPSET_VIRGL, _VIRGL2, _VENUS
constexpr uint32_t PIPE_TEXTURE_2D = 2, BIND_RENDER_TARGET = 1u << 1;
constexpr uint32_t RESOURCE_FLAG_Y_0_TOP = 1;   // VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP, and virgl's info flag

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

const uint8_t *swizzle(uint32_t format)
{
	static const uint8_t bgrx[4] = {0, 1, 2, 3}, xrgb[4] = {3, 2, 1, 0}, rgbx[4] = {2, 1, 0, 3},
	                     xbgr[4] = {1, 2, 3, 0};
	switch (format) {
	case 1: case 2:     return bgrx;
	case 3: case 4:     return xrgb;
	case 67: case 134:  return rgbx;
	case 68: case 121:  return xbgr;
	default:            return nullptr;
	}
}

} // namespace

VirglBackend *VirtioGpu::host()
{
	if (!backend_tried) {
		backend_tried = true;
		auto b = std::make_unique<VirglBackend>();
		std::string error;
		if (b->start(error, venus)) backend = std::move(b);
		else std::cout << "gpu: no 3D on this host: " << error << std::endl;
	}
	return backend.get();
}

void VirtioGpu::command_virgl(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, Writes &writes, Memory &mem)
{
	const uint32_t type = le32(in, 0), ctx = le32(in, 16);
	out.assign(HDR, 0);
	uint32_t resp = RESP_OK_NODATA;
	VirglBackend *v = type == CMD_GET_DISPLAY_INFO ? nullptr : host();

	if (type == CMD_GET_DISPLAY_INFO) {
		resp = RESP_OK_DISPLAY_INFO;
		out.resize(HDR + MAX_SCANOUTS * 24, 0);
		put32(out, HDR + 8, width);
		put32(out, HDR + 12, height);
		put32(out, HDR + 16, 1);
	} else if (!v) {
		resp = RESP_ERR_UNSPEC;
	} else switch (type) {
	case CMD_GET_CAPSET_INFO: {
		const uint32_t index = le32(in, 24);
		if (index >= (venus ? 3u : 2u)) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		uint32_t max_ver = 0, max_size = 0;
		if (CAPSETS[index] == CAPSET_VENUS) max_size = (uint32_t)v->venus_capset(nullptr);
		else v->get_cap_set(CAPSETS[index], &max_ver, &max_size);
		resp = RESP_OK_CAPSET_INFO;
		put32(out, HDR, CAPSETS[index]);
		put32(out, HDR + 4, max_ver);
		put32(out, HDR + 8, max_size);
		put32(out, HDR + 12, 0);
		break;
	}
	case CMD_GET_CAPSET: {
		const uint32_t id = le32(in, 24), version = le32(in, 28);
		uint32_t max_ver = 0, max_size = 0;
		if (id == CAPSET_VENUS && venus) max_size = (uint32_t)v->venus_capset(nullptr);
		else v->get_cap_set(id, &max_ver, &max_size);
		if (!max_size || max_size > (1u << 20)) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		resp = RESP_OK_CAPSET;
		out.resize(HDR + max_size, 0);
		if (id == CAPSET_VENUS && venus) v->venus_capset(out.data() + HDR);
		else v->fill_caps(id, version, out.data() + HDR);
		break;
	}
	case CMD_RESOURCE_CREATE_2D:
	case CMD_RESOURCE_CREATE_3D: {
		VirglResourceArgs args{};
		args.handle = le32(in, 24);
		if (type == CMD_RESOURCE_CREATE_2D) {
			args.target = PIPE_TEXTURE_2D;
			args.format = le32(in, 28);
			args.bind = BIND_RENDER_TARGET;
			args.width = le32(in, 32);
			args.height = le32(in, 36);
			args.depth = 1;
			args.array_size = 1;
			args.flags = RESOURCE_FLAG_Y_0_TOP;
		} else {
			// target, format, bind, width, height, depth, array_size,
			// last_level, nr_samples, flags.
			args.target = le32(in, 28);
			args.format = le32(in, 32);
			args.bind = le32(in, 36);
			args.width = le32(in, 40);
			args.height = le32(in, 44);
			args.depth = le32(in, 48);
			args.array_size = le32(in, 52);
			args.last_level = le32(in, 56);
			args.nr_samples = le32(in, 60);
			args.flags = le32(in, 64);
		}
		if (args.handle == 0) { resp = RESP_ERR_INVALID_RESOURCE_ID; break; }
		if (v->resource_create(&args, nullptr, 0) != 0) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		virgl_resources.push_back(args.handle);
		break;
	}
	case CMD_RESOURCE_UNREF: {
		const uint32_t id = le32(in, 24);
		const auto blob = venus_blobs.find(id);
		if (blob != venus_blobs.end()) {
			for (auto m = mappings.begin(); m != mappings.end();)
				m = m->second.resource == id ? mappings.erase(m) : std::next(m);
			v->venus_destroy_resource(blob->second.first, id);
			venus_blobs.erase(blob);
			break;
		}
		const auto it = std::find(virgl_resources.begin(), virgl_resources.end(), id);
		if (it == virgl_resources.end()) { resp = RESP_ERR_INVALID_RESOURCE_ID; break; }
		struct iovec *iov = nullptr;
		int n = 0;
		v->resource_detach_iov((int)id, &iov, &n);
		backings.erase(id);
		v->resource_unref(id);
		virgl_resources.erase(it);
		if (scanout_resource == id) {
			scanout_resource = 0;
			blank(mem);
		}
		break;
	}
	case CMD_RESOURCE_ATTACH_BACKING: {
		const uint32_t id = le32(in, 24), n = le32(in, 28);
		if (32 + (uint64_t)n * 16 > in.size()) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		Backing b;
		for (uint32_t i = 0; i < n; i++) {
			const uint64_t addr = le64(in, 32 + i * 16);
			const uint32_t len = le32(in, 40 + i * 16);
			// virglrenderer reads and writes the pages itself, through host
			// pointers: so they must be RAM.
			if (!Memory::in_ram(addr, len)) { resp = RESP_ERR_INVALID_PARAMETER; break; }
			b.iov.push_back({mem.ram_data_mut() + (addr - Memory::RAM_BASE), len});
			b.guest.push_back(addr);
		}
		if (resp != RESP_OK_NODATA) break;
		Backing &kept = backings[id];
		kept = std::move(b);
		if (v->resource_attach_iov((int)id, kept.iov.data(), (int)kept.iov.size()) != 0) {
			backings.erase(id);
			resp = RESP_ERR_INVALID_RESOURCE_ID;
		}
		break;
	}
	case CMD_RESOURCE_DETACH_BACKING: {
		const uint32_t id = le32(in, 24);
		struct iovec *iov = nullptr;
		int n = 0;
		v->resource_detach_iov((int)id, &iov, &n);
		backings.erase(id);
		break;
	}
	case CMD_SET_SCANOUT: {
		const uint32_t x = le32(in, 24), y = le32(in, 28), scanout = le32(in, 40), id = le32(in, 44);
		if (scanout != 0) { resp = RESP_ERR_INVALID_SCANOUT_ID; break; }
		if (id == 0) {
			scanout_resource = 0;
			blank(mem);
			break;
		}
		VirglResourceInfo info{};
		if (v->resource_get_info((int)id, &info) != 0) { resp = RESP_ERR_INVALID_RESOURCE_ID; break; }
		scanout_resource = id;
		scanout_x = x;
		scanout_y = y;
		present_virgl(mem);
		break;
	}
	case CMD_RESOURCE_FLUSH:
		if (le32(in, 40) == scanout_resource) present_virgl(mem);
		break;
	case CMD_TRANSFER_TO_HOST_2D: {
		virgl_box box{le32(in, 24), le32(in, 28), 0, le32(in, 32), le32(in, 36), 1};
		if (v->transfer_write_iov(le32(in, 48), 0, 0, 0, 0, &box, le64(in, 40), nullptr, 0) != 0)
			resp = RESP_ERR_INVALID_PARAMETER;
		break;
	}
	case CMD_TRANSFER_TO_HOST_3D:
	case CMD_TRANSFER_FROM_HOST_3D: {
		// box (x, y, z, w, h, d), offset, resource_id, level, stride,
		// layer_stride.
		virgl_box box{le32(in, 24), le32(in, 28), le32(in, 32), le32(in, 36), le32(in, 40), le32(in, 44)};
		const uint64_t offset = le64(in, 48);
		const uint32_t id = le32(in, 56), level = le32(in, 60), stride = le32(in, 64), layer_stride = le32(in, 68);
		if (type == CMD_TRANSFER_TO_HOST_3D) {
			if (v->transfer_write_iov(id, ctx, (int)level, stride, layer_stride, &box, offset, nullptr, 0) != 0)
				resp = RESP_ERR_INVALID_PARAMETER;
			break;
		}
		// A read-back writes guest memory with what the host GPU computed:
		// what changed is taken down as part of the result.
		const auto it = backings.find(id);
		std::vector<std::vector<uint8_t>> before;
		if (it != backings.end())
			for (const auto &io : it->second.iov) {
				const uint8_t *p = (const uint8_t *)io.iov_base;
				before.emplace_back(p, p + io.iov_len);
			}
		if (v->transfer_read_iov(id, ctx, level, stride, layer_stride, &box, offset, nullptr, 0) != 0)
			resp = RESP_ERR_INVALID_PARAMETER;
		if (it == backings.end()) break;
		for (size_t e = 0; e < it->second.iov.size(); e++) {
			const uint8_t *now = (const uint8_t *)it->second.iov[e].iov_base;
			const size_t len = it->second.iov[e].iov_len;
			for (size_t i = 0; i < len;) {
				if (now[i] == before[e][i]) { i++; continue; }
				size_t j = i;
				while (j < len && (now[j] != before[e][j] || (j + 1 < len && now[j + 1] != before[e][j + 1]))) j++;
				writes.push_back({it->second.guest[e] + i, std::vector<uint8_t>(now + i, now + j)});
				i = j;
			}
		}
		break;
	}
	case CMD_CTX_CREATE: {
		// nlen, context_init, debug_name[64].
		const uint32_t nlen = std::min<uint32_t>(le32(in, 24), 64), init = le32(in, 28);
		char name[65] = {};
		if (in.size() >= 32 + nlen) std::memcpy(name, in.data() + 32, nlen);
		if (venus && (init & 0xff) == CAPSET_VENUS) {
			if (v->venus_create_context(ctx, init & 0xff, nlen, name)) venus_contexts.push_back(ctx);
			else resp = RESP_ERR_UNSPEC;
			break;
		}
		const int rc = init ? v->context_create_with_flags(ctx, init, nlen, name) : v->context_create(ctx, nlen, name);
		if (rc != 0) resp = RESP_ERR_UNSPEC;
		break;
	}
	case CMD_CTX_DESTROY:
		if (is_venus_context(ctx)) {
			v->venus_step();   // anything it left running finishes first
			v->venus_destroy_context(ctx);
			venus_contexts.erase(std::find(venus_contexts.begin(), venus_contexts.end(), ctx));
		} else {
			v->context_destroy(ctx);
		}
		break;
	case CMD_CTX_ATTACH_RESOURCE:
		if (!is_venus_context(ctx)) v->ctx_attach_resource((int)ctx, (int)le32(in, 24));
		break;
	case CMD_CTX_DETACH_RESOURCE:
		if (!is_venus_context(ctx)) v->ctx_detach_resource((int)ctx, (int)le32(in, 24));
		break;
	case CMD_RESOURCE_CREATE_BLOB: {
		// resource_id, blob_mem, blob_flags, nr_entries, blob_id, size.
		// Venus's: shared memory (blob_id 0) or a VkDeviceMemory, on the host.
		const uint32_t id = le32(in, 24), blob_mem = le32(in, 28), flags = le32(in, 32);
		const uint64_t blob_id = le64(in, 40), size = le64(in, 48);
		uint32_t map_info = 0;
		if (!is_venus_context(ctx) || blob_mem != BLOB_MEM_HOST3D) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		if (!id || venus_blobs.count(id)) { resp = RESP_ERR_INVALID_RESOURCE_ID; break; }
		if (!v->venus_create_resource(ctx, id, blob_id, size, flags, &map_info)) { resp = RESP_ERR_OUT_OF_MEMORY; break; }
		venus_blobs[id] = {ctx, map_info};
		break;
	}
	case CMD_RESOURCE_MAP_BLOB: {
		// resource_id, padding, offset in the host-memory window.
		const uint32_t id = le32(in, 24);
		const uint64_t offset = le64(in, 32);
		const auto blob = venus_blobs.find(id);
		void *ptr = nullptr;
		uint64_t size = 0;
		if (blob == venus_blobs.end() || !v->venus_map_resource(blob->second.first, id, &ptr, &size)) {
			resp = RESP_ERR_INVALID_RESOURCE_ID;
			break;
		}
		if (offset > HOSTMEM_SIZE || size > HOSTMEM_SIZE - offset) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		mappings[offset] = {(uint8_t *)ptr, size, id};
		resp = RESP_OK_MAP_INFO;
		put32(out, HDR, blob->second.second);
		put32(out, HDR + 4, 0);
		break;
	}
	case CMD_RESOURCE_UNMAP_BLOB: {
		const uint32_t id = le32(in, 24);
		for (auto m = mappings.begin(); m != mappings.end();)
			m = m->second.resource == id ? mappings.erase(m) : std::next(m);
		break;
	}
	case CMD_SUBMIT_3D: {
		const uint32_t size = le32(in, 24);
		if (size % 4 || 32 + (uint64_t)size > in.size()) { resp = RESP_ERR_INVALID_PARAMETER; break; }
		std::vector<uint32_t> cmds(size / 4);   // aligned, as virglrenderer wants
		std::memcpy(cmds.data(), in.data() + 32, size);
		if (is_venus_context(ctx)) {
			if (!v->venus_submit_cmd(ctx, cmds.data(), size)) resp = RESP_ERR_INVALID_PARAMETER;
			// A notify of a ring, or a command that waits on one: whatever it
			// started is run and finished now, at this instruction.
			v->venus_step();
		} else if (v->submit_cmd(cmds.data(), (int)ctx, (int)(size / 4)) != 0) {
			resp = RESP_ERR_INVALID_PARAMETER;
		}
		break;
	}
	default:
		resp = RESP_ERR_UNSPEC;
		break;
	}

	put32(out, 0, resp);
	const uint32_t flags = le32(in, 4);
	if (flags & FLAG_FENCE) {
		// Fenced: answered once the host GPU has done everything so far. A
		// Venus fence on a ring is retired by Venus, inside the call.
		const uint32_t ring_idx = in.size() > 20 ? in[20] : 0;
		if (v && is_venus_context(ctx) && (flags & FLAG_INFO_RING_IDX)) {
			v->venus_step();
			v->venus_submit_fence(ctx, 0, ring_idx, le64(in, 8));
		} else if (v) {
			v->finish(0);
		}
		put32(out, 4, flags & (FLAG_FENCE | FLAG_INFO_RING_IDX));
		std::memcpy(out.data() + 8, in.data() + 8, 8);
		std::memcpy(out.data() + 16, in.data() + 16, 4);
		out[20] = (uint8_t)ring_idx;
	}
}

bool VirtioGpu::is_venus_context(uint32_t ctx) const
{
	return venus && std::find(venus_contexts.begin(), venus_contexts.end(), ctx) != venus_contexts.end();
}

uint8_t *VirtioGpu::hostmem(uint64_t offset, uint64_t len) const
{
	auto it = mappings.upper_bound(offset);
	if (it == mappings.begin()) return nullptr;
	--it;
	const uint64_t into = offset - it->first;
	if (into >= it->second.size || len > it->second.size - into) return nullptr;
	return it->second.ptr + into;
}

void VirtioGpu::step()
{
	if (backend) backend->venus_step();
}

// The scanout's resource, read back from the host GPU into the window's
// buffer. Only the window sees it; the guest never reads this.
void VirtioGpu::present_virgl(Memory &mem)
{
	VirglBackend *v = host();
	VirglResourceInfo info{};
	if (!v || !scanout_resource || v->resource_get_info((int)scanout_resource, &info) != 0) return;
	const uint8_t *map = swizzle(info.virgl_format);
	if (!map || scanout_x >= info.width || scanout_y >= info.height) return;
	const uint32_t w = std::min(info.width - scanout_x, width), h = std::min(info.height - scanout_y, height);
	readback.resize((size_t)w * h * 4);
	const bool top_down = info.flags & RESOURCE_FLAG_Y_0_TOP;
	// Rows count from the bottom in a resource that is not top-down.
	virgl_box box{scanout_x, top_down ? scanout_y : info.height - scanout_y - h, 0, w, h, 1};
	struct iovec iov{readback.data(), readback.size()};
	v->force_ctx_0();
	if (v->transfer_read_iov(scanout_resource, 0, 0, w * 4, 0, &box, 0, &iov, 1) != 0) return;
	(void)mem;
	uint8_t *screen = screen_buf.data();
	for (uint32_t row = 0; row < h; row++) {
		const uint8_t *src = readback.data() + (size_t)(top_down ? row : h - 1 - row) * w * 4;
		uint8_t *dst = screen + (size_t)row * width * 4;
		for (uint32_t i = 0; i < w; i++, src += 4, dst += 4) {
			dst[0] = src[map[0]]; dst[1] = src[map[1]]; dst[2] = src[map[2]]; dst[3] = 0;
		}
	}
	screen_gen.fetch_add(1, std::memory_order_relaxed);
}
