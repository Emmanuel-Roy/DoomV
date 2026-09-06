#include "guest_ram.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <securitybaseapi.h>
#include <processthreadsapi.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

const char *GuestRam::name(Backend b)
{
	switch (b) {
	case Backend::Vector:    return "vector";
	case Backend::Pages:     return "pages";
	case Backend::HugePages: return "hugepages";
	}
	return "?";
}

GuestRam::Backend GuestRam::from_env()
{
	const char *v = std::getenv("DOOMV_RAM_BACKEND");
	if (!v) return Backend::HugePages;
	const std::string s(v);
	if (s == "vector")    return Backend::Vector;
	if (s == "pages")     return Backend::Pages;
	if (s == "hugepages") return Backend::HugePages;
	return Backend::HugePages;
}

namespace {

#ifdef _WIN32
// Large pages on Windows are gated on a privilege the account has to hold --
// "Lock pages in memory", which is not granted by default. Ask for it once;
// if it is not there to be enabled, AdjustTokenPrivileges succeeds but
// GetLastError reports that not all privileges were assigned, which is the
// only way it tells you.
bool enable_lock_memory_privilege()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
		return false;
	TOKEN_PRIVILEGES tp{};
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	bool ok = LookupPrivilegeValueA(nullptr, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid) != 0;
	if (ok) {
		AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
		ok = GetLastError() == ERROR_SUCCESS;
	}
	CloseHandle(token);
	return ok;
}
#endif

} // namespace

void GuestRam::allocate(size_t bytes, Backend wanted)
{
	release();
	length = bytes;

	if (wanted == Backend::Vector) {
		heap.assign(bytes, 0);
		ptr = heap.data();
		active_backend = Backend::Vector;
		return;
	}

#ifdef _WIN32
	if (wanted == Backend::HugePages && enable_lock_memory_privilege()) {
		const SIZE_T large = GetLargePageMinimum();
		if (large != 0) {
			// MEM_LARGE_PAGES needs the length rounded up to the large page
			// size, and commits immediately -- there is no lazy path for it.
			const SIZE_T rounded = (bytes + large - 1) / large * large;
			void *p = VirtualAlloc(nullptr, rounded, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
			                       PAGE_READWRITE);
			if (p) {
				ptr = (uint8_t *)p;
				active_backend = Backend::HugePages;
				return;
			}
		}
	}
	// Ordinary pages: reserved and committed up front, but the physical
	// pages arrive on first touch and arrive zeroed.
	if (void *p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
		ptr = (uint8_t *)p;
		active_backend = Backend::Pages;
		return;
	}
#else
	void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p != MAP_FAILED) {
		ptr = (uint8_t *)p;
		active_backend = Backend::Pages;
#ifdef MADV_HUGEPAGE
		// Advisory: the kernel gives large pages where it can and quietly
		// does not where it cannot, so a success here is not proof. Reported
		// as HugePages because that is what was asked for and what the
		// mapping is eligible for; /proc/meminfo's AnonHugePages is what
		// says whether it happened.
		if (wanted == Backend::HugePages && madvise(p, bytes, MADV_HUGEPAGE) == 0)
			active_backend = Backend::HugePages;
#endif
		return;
	}
#endif

	// Nothing the OS offered worked; the heap is still a correct answer.
	heap.assign(bytes, 0);
	ptr = heap.data();
	active_backend = Backend::Vector;
}

void GuestRam::release()
{
	if (ptr && active_backend != Backend::Vector) {
#ifdef _WIN32
		VirtualFree(ptr, 0, MEM_RELEASE);
#else
		munmap(ptr, length);
#endif
	}
	heap.clear();
	heap.shrink_to_fit();
	ptr = nullptr;
	length = 0;
}

GuestRam::~GuestRam() { release(); }
