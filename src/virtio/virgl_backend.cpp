#include "virgl_backend.hpp"
#include <windows.h>
#include <GL/gl.h>
#include <filesystem>
#include <iostream>

// virgl_renderer_gl_ctx_param, and the callbacks structure, as virglrenderer.h
// lays them out (callbacks version 1: fences and the three context calls).
struct VirglCtxParam {
	int version;
	bool shared;
	int major_ver;
	int minor_ver;
	int compat_ctx;
};

namespace {

struct Callbacks {
	int version;
	void (*write_fence)(void *, uint32_t);
	void *(*create_gl_context)(void *, int, VirglCtxParam *);
	void (*destroy_gl_context)(void *, void *);
	int (*make_current)(void *, int, void *);
};

// WGL_ARB_create_context and _profile.
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091, WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092,
              WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126, WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x1,
              WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x2;

std::filesystem::path library_path(bool venus)
{
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	return std::filesystem::path(exe).parent_path() / "build" / (venus ? "venus" : "virgl") / "bin" /
	       "libvirglrenderer-1.dll";
}

// A Venus fence is retired inside the call that submitted it (the synchronous
// mode has no thread to retire it later), so there is nothing to do here.
void venus_retired(uint32_t, uint32_t, uint64_t) {}

} // namespace

void VirglBackend::write_fence(void *cookie, uint32_t fence)
{
	static_cast<VirglBackend *>(cookie)->fence_done = fence;
}

void *VirglBackend::create_gl_context(void *cookie, int, VirglCtxParam *param)
{
	VirglBackend *self = static_cast<VirglBackend *>(cookie);
	const int attribs[] = {
		WGL_CONTEXT_MAJOR_VERSION_ARB, param->major_ver,
		WGL_CONTEXT_MINOR_VERSION_ARB, param->minor_ver,
		WGL_CONTEXT_PROFILE_MASK_ARB, param->compat_ctx ? WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
		                                                : WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
		0,
	};
	// Every context shares with the first, which is what "shared" asks for,
	// and costs nothing when it was not asked.
	return self->create_context_attribs(self->dc, self->root, attribs);
}

void VirglBackend::destroy_gl_context(void *, void *ctx)
{
	if (ctx) wglDeleteContext((HGLRC)ctx);
}

int VirglBackend::make_current(void *cookie, int, void *ctx)
{
	VirglBackend *self = static_cast<VirglBackend *>(cookie);
	return wglMakeCurrent((HDC)self->dc, (HGLRC)ctx) ? 0 : -1;
}

bool VirglBackend::start(std::string &error, bool venus)
{
	const std::filesystem::path path = library_path(venus);
	HMODULE lib = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!lib) {
		error = "cannot load " + path.string() + (venus ? " (build it with: python scripts/get_venus.py)"
		                                                : " (fetch it with: python scripts/get_virgl.py)");
		return false;
	}
	dll = lib;
	bool missing = false;
	const auto get = [&](auto &fn, const char *name) {
		fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(lib, name));
		if (!fn) {
			error = std::string("virglrenderer has no ") + name;
			missing = true;
		}
	};
	int (*init)(void *, int, Callbacks *) = nullptr;
	get(init, "virgl_renderer_init");
	get(resource_create, "virgl_renderer_resource_create");
	get(resource_unref, "virgl_renderer_resource_unref");
	get(resource_attach_iov, "virgl_renderer_resource_attach_iov");
	get(resource_detach_iov, "virgl_renderer_resource_detach_iov");
	get(resource_get_info, "virgl_renderer_resource_get_info");
	get(context_create, "virgl_renderer_context_create");
	get(context_create_with_flags, "virgl_renderer_context_create_with_flags");
	get(context_destroy, "virgl_renderer_context_destroy");
	get(ctx_attach_resource, "virgl_renderer_ctx_attach_resource");
	get(ctx_detach_resource, "virgl_renderer_ctx_detach_resource");
	get(submit_cmd, "virgl_renderer_submit_cmd");
	get(transfer_read_iov, "virgl_renderer_transfer_read_iov");
	get(transfer_write_iov, "virgl_renderer_transfer_write_iov");
	get(get_cap_set, "virgl_renderer_get_cap_set");
	get(fill_caps, "virgl_renderer_fill_caps");
	get(create_fence, "virgl_renderer_create_fence");
	get(poll, "virgl_renderer_poll");
	get(force_ctx_0, "virgl_renderer_force_ctx_0");
	bool (*venus_init)(void (*)(uint32_t, uint32_t, uint64_t)) = nullptr;
	if (venus) {
		get(venus_init, "virgl_doomv_venus_init");
		get(venus_capset, "virgl_doomv_venus_capset");
		get(venus_create_context, "virgl_doomv_venus_create_context");
		get(venus_destroy_context, "virgl_doomv_venus_destroy_context");
		get(venus_submit_cmd, "virgl_doomv_venus_submit_cmd");
		get(venus_submit_fence, "virgl_doomv_venus_submit_fence");
		get(venus_create_resource, "virgl_doomv_venus_create_resource");
		get(venus_map_resource, "virgl_doomv_venus_map_resource");
		get(venus_destroy_resource, "virgl_doomv_venus_destroy_resource");
		get(venus_step, "virgl_doomv_venus_step");
	}
	if (missing) return false;

	// A hidden window for a device context with an OpenGL pixel format: WGL
	// has no other way to make a context.
	WNDCLASSW wc{};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"DoomVVirgl";
	RegisterClassW(&wc);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
	                            nullptr, nullptr, wc.hInstance, nullptr);
	HDC hdc = hwnd ? GetDC(hwnd) : nullptr;
	PIXELFORMATDESCRIPTOR pfd{};
	pfd.nSize = sizeof pfd;
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	pfd.cStencilBits = 8;
	const int format = hdc ? ChoosePixelFormat(hdc, &pfd) : 0;
	if (!format || !SetPixelFormat(hdc, format, &pfd)) {
		error = "no OpenGL pixel format on this host";
		return false;
	}
	window = hwnd;
	dc = hdc;
	// A legacy context first, which is the only way to reach
	// wglCreateContextAttribsARB, then the real one.
	HGLRC legacy = wglCreateContext(hdc);
	if (!legacy || !wglMakeCurrent(hdc, legacy)) {
		error = "cannot create an OpenGL context";
		return false;
	}
	create_context_attribs = reinterpret_cast<decltype(create_context_attribs)>(
		wglGetProcAddress("wglCreateContextAttribsARB"));
	if (!create_context_attribs) {
		error = "the host's OpenGL has no WGL_ARB_create_context";
		return false;
	}
	static const int versions[][2] = {{4, 6}, {4, 3}, {3, 3}};
	for (const auto &v : versions) {
		const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, v[0], WGL_CONTEXT_MINOR_VERSION_ARB, v[1],
		                       WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
		root = create_context_attribs(hdc, nullptr, attribs);
		if (root) break;
	}
	wglMakeCurrent(hdc, nullptr);
	wglDeleteContext(legacy);
	if (!root || !wglMakeCurrent(hdc, (HGLRC)root)) {
		error = "the host's OpenGL has no 3.3 core context";
		return false;
	}

	static Callbacks cb{1, &VirglBackend::write_fence, &VirglBackend::create_gl_context,
	                    &VirglBackend::destroy_gl_context, &VirglBackend::make_current};
	if (init(this, 0, &cb) != 0) {
		error = "virgl_renderer_init failed";
		return false;
	}
	if (venus && !venus_init(venus_retired)) {
		error = "Venus did not start (no Vulkan on this host?)";
		return false;
	}
	ready = true;
	const char *vendor = (const char *)glGetString(GL_VENDOR), *renderer = (const char *)glGetString(GL_RENDERER);
	std::cout << "gpu: virgl on " << (vendor ? vendor : "?") << " " << (renderer ? renderer : "?") << std::endl;
	return true;
}

bool VirglBackend::finish(uint32_t ctx_id)
{
	if (!ready) return false;
	const uint32_t fence = next_fence++;
	if (create_fence((int)fence, ctx_id) != 0) return false;
	// virglrenderer reports a fence from poll() once the host GPU has passed
	// it; there is no thread here to wake, so this waits for it.
	for (int spins = 0; fence_done != fence; spins++) {
		poll();
		if (fence_done == fence) break;
		if (spins > 2000000) return false;   // a GPU that never finishes: give up rather than hang
		if (spins > 1000) Sleep(0);
	}
	return true;
}
