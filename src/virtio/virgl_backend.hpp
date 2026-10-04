#pragma once
// The host side of -gpu=virgl: virglrenderer, which turns the guest's Gallium
// command streams into OpenGL on the host GPU, and the OpenGL contexts it
// draws with.
//
// virglrenderer is loaded at run time from build/virgl/ (scripts/
// get_virgl.py), so a DoomV without it builds and runs as before and only
// -gpu=virgl needs it. On Windows it cannot make contexts of its own -- it
// has EGL and GLX backends, neither of which exists here -- so they come from
// here, through WGL, on a hidden window. Everything runs on the CPU thread:
// that is where the device's commands are carried out, and an OpenGL context
// belongs to one thread.
#include <cstddef>
#include <cstdint>
#include <string>

// The two structures virglrenderer's API takes by pointer and leaves to the
// caller to define: the Windows layout of POSIX's iovec, and Gallium's box.
struct iovec {
	void *iov_base;
	size_t iov_len;
};
struct virgl_box {
	uint32_t x, y, z, w, h, d;
};

struct VirglResourceArgs {
	uint32_t handle, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
};
struct VirglResourceInfo {
	uint32_t handle, virgl_format, width, height, depth, flags, tex_id, stride;
	int drm_fourcc, fd;
};

class VirglBackend {
public:
	// Loads virglrenderer and makes the first context. False, with the reason
	// in `error`, if either cannot be done; the GPU then answers every 3D
	// command with an error, as a host without the hardware would.
	bool start(std::string &error);
	bool started() const { return ready; }

	// virglrenderer's own functions, resolved from the DLL.
	int (*resource_create)(VirglResourceArgs *, struct iovec *, uint32_t) = nullptr;
	void (*resource_unref)(uint32_t) = nullptr;
	int (*resource_attach_iov)(int, struct iovec *, int) = nullptr;
	void (*resource_detach_iov)(int, struct iovec **, int *) = nullptr;
	int (*resource_get_info)(int, VirglResourceInfo *) = nullptr;
	int (*context_create)(uint32_t, uint32_t, const char *) = nullptr;
	int (*context_create_with_flags)(uint32_t, uint32_t, uint32_t, const char *) = nullptr;
	void (*context_destroy)(uint32_t) = nullptr;
	void (*ctx_attach_resource)(int, int) = nullptr;
	void (*ctx_detach_resource)(int, int) = nullptr;
	int (*submit_cmd)(void *, int, int) = nullptr;
	int (*transfer_read_iov)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, virgl_box *, uint64_t, struct iovec *, int) = nullptr;
	int (*transfer_write_iov)(uint32_t, uint32_t, int, uint32_t, uint32_t, virgl_box *, uint64_t, struct iovec *, unsigned) = nullptr;
	void (*get_cap_set)(uint32_t, uint32_t *, uint32_t *) = nullptr;
	void (*fill_caps)(uint32_t, uint32_t, void *) = nullptr;
	int (*create_fence)(int, uint32_t) = nullptr;
	void (*poll)() = nullptr;
	void (*force_ctx_0)() = nullptr;

	// Waits until everything submitted so far has finished on the host GPU:
	// how a fenced command is made complete by the time it is answered.
	bool finish(uint32_t ctx_id);

private:
	bool ready = false;
	uint32_t next_fence = 1;
	uint32_t fence_done = 0;
	void *dll = nullptr;
	void *window = nullptr, *dc = nullptr, *root = nullptr;
	void *(*create_context_attribs)(void *, void *, const int *) = nullptr;

	static void write_fence(void *cookie, uint32_t fence);
	static void *create_gl_context(void *cookie, int scanout, struct VirglCtxParam *param);
	static void destroy_gl_context(void *cookie, void *ctx);
	static int make_current(void *cookie, int scanout, void *ctx);
};
