#include "virtio_9p.hpp"
#include "aplic.hpp"
#include "memory.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>

#ifdef _WIN32
// This MinGW targets Windows XP by default, which hides the Vista-era
// GetFinalPathNameByHandleW the escape check relies on. Vista is far below
// anything this runs on.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr uint16_t DESC_F_NEXT  = 1;
constexpr uint16_t DESC_F_WRITE = 2;
constexpr uint64_t DESC_SIZE    = 16;

void write16_phys(Memory &mem, uint64_t addr, uint16_t v)
{
	mem.write8(addr, (uint8_t)(v & 0xFF));
	mem.write8(addr + 1, (uint8_t)(v >> 8));
}

// 9P2000.L message types. Each reply is its request's type plus one.
enum : uint8_t {
	Rlerror    = 7,
	Tstatfs    = 8,
	Tlopen     = 12,
	Tlcreate   = 14,
	Tsymlink   = 16,
	Tmknod     = 18,
	Trename    = 20,
	Treadlink  = 22,
	Tgetattr   = 24,
	Tsetattr   = 26,
	Txattrwalk = 30,
	Txattrcreate = 32,
	Treaddir   = 40,
	Tfsync     = 50,
	Tlock      = 52,
	Tgetlock   = 54,
	Tlink      = 70,
	Tmkdir     = 72,
	Trenameat  = 74,
	Tunlinkat  = 76,
	Tversion   = 100,
	Tauth      = 102,
	Tattach    = 104,
	Tflush     = 108,
	Twalk      = 110,
	Tread      = 116,
	Twrite     = 118,
	Tclunk     = 120,
	Tremove    = 122,
};

// Linux errno values -- the guest's numbers, not the host C library's, which
// numbers several of these differently (ENOTEMPTY is 41 there, 39 here).
enum : uint32_t {
	L_EPERM        = 1,
	L_ENOENT       = 2,
	L_EIO          = 5,
	L_EBADF        = 9,
	L_EACCES       = 13,
	L_EBUSY        = 16,
	L_EEXIST       = 17,
	L_EXDEV        = 18,
	L_ENOTDIR      = 20,
	L_EISDIR       = 21,
	L_EINVAL       = 22,
	L_ENOSPC       = 28,
	L_EROFS        = 30,
	L_ENAMETOOLONG = 36,
	L_ENOTEMPTY    = 39,
	L_EOPNOTSUPP   = 95,
};

// Linux open(2) flags, as the client sends them.
constexpr uint32_t L_O_ACCMODE = 03;
constexpr uint32_t L_O_WRONLY  = 01;
constexpr uint32_t L_O_RDWR    = 02;
constexpr uint32_t L_O_EXCL    = 0200;
constexpr uint32_t L_O_TRUNC   = 01000;
constexpr uint32_t L_O_APPEND  = 02000;

constexpr uint32_t P9_SETATTR_MODE      = 0x001;
constexpr uint32_t P9_SETATTR_SIZE      = 0x008;
constexpr uint32_t P9_SETATTR_ATIME     = 0x010;
constexpr uint32_t P9_SETATTR_MTIME     = 0x020;
constexpr uint32_t P9_SETATTR_ATIME_SET = 0x080;
constexpr uint32_t P9_SETATTR_MTIME_SET = 0x100;
constexpr uint32_t AT_REMOVEDIR         = 0x200;
constexpr uint8_t  QTDIR  = 0x80;
constexpr uint8_t  DT_DIR = 4;
constexpr uint8_t  DT_REG = 8;
constexpr uint32_t NOFID  = 0xFFFFFFFFu;

struct Qid {
	uint8_t type;
	uint32_t version;
	uint64_t path;
};

// Little-endian readers and writers over a message buffer. A reader that
// runs off the end stops and remembers it, so a short or malformed request
// becomes an EINVAL reply rather than a read past the buffer.
struct Reader {
	const std::vector<uint8_t> &b;
	size_t p;
	bool bad = false;

	bool need(size_t n)
	{
		if (p + n > b.size()) { bad = true; return false; }
		return true;
	}
	uint64_t le(int bytes)
	{
		if (!need((size_t)bytes)) return 0;
		uint64_t v = 0;
		for (int i = 0; i < bytes; i++) v |= (uint64_t)b[p + i] << (8 * i);
		p += (size_t)bytes;
		return v;
	}
	uint8_t  u8()  { return (uint8_t)le(1); }
	uint16_t u16() { return (uint16_t)le(2); }
	uint32_t u32() { return (uint32_t)le(4); }
	uint64_t u64() { return le(8); }
	std::string str()
	{
		const uint16_t n = u16();
		if (!need(n)) return std::string();
		std::string s(reinterpret_cast<const char *>(b.data() + p), n);
		p += n;
		return s;
	}
};

struct Writer {
	std::vector<uint8_t> &b;

	void le(uint64_t v, int bytes)
	{
		for (int i = 0; i < bytes; i++) b.push_back((uint8_t)(v >> (8 * i)));
	}
	void u8(uint8_t v)   { le(v, 1); }
	void u16(uint16_t v) { le(v, 2); }
	void u32(uint32_t v) { le(v, 4); }
	void u64(uint64_t v) { le(v, 8); }
	void str(const std::string &s)
	{
		u16((uint16_t)s.size());
		b.insert(b.end(), s.begin(), s.end());
	}
	void qid(const Qid &q)
	{
		u8(q.type);
		u32(q.version);
		u64(q.path);
	}
};

void begin_reply(std::vector<uint8_t> &resp, uint8_t type, uint16_t tag)
{
	resp.clear();
	Writer w{resp};
	w.u32(0);          // size, filled in by finish_reply
	w.u8(type);
	w.u16(tag);
}

void finish_reply(std::vector<uint8_t> &resp)
{
	const uint32_t n = (uint32_t)resp.size();
	for (int i = 0; i < 4; i++) resp[(size_t)i] = (uint8_t)(n >> (8 * i));
}

void error_reply(std::vector<uint8_t> &resp, uint16_t tag, uint32_t ecode)
{
	begin_reply(resp, Rlerror, tag);
	Writer{resp}.u32(ecode);
	finish_reply(resp);
}

std::string join(const std::string &dir, const std::string &name)
{
	return dir.empty() ? name : dir + "/" + name;
}

std::string parent_of(const std::string &path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Whether `name` is one path element the host can hold under exactly that
// name. Windows rejects some characters outright, and silently changes
// others -- a trailing dot or space is stripped, and CON or NUL name a
// device rather than a file -- so those are refused here instead of being
// allowed to alias a different file.
bool valid_name(const std::string &name)
{
	if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
	for (unsigned char c : name) {
		if (c < 0x20 || std::strchr("/\\:*?\"<>|", c)) return false;
	}
	if (name.back() == '.' || name.back() == ' ') return false;
	std::string base = name.substr(0, name.find('.'));
	for (char &c : base) c = (char)std::toupper((unsigned char)c);
	static const char *reserved[] = {"CON", "PRN", "AUX", "NUL",
		"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
		"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
	for (const char *r : reserved)
		if (base == r) return false;
	return true;
}

} // namespace

#ifdef _WIN32

namespace {

bool widen(const std::string &s, std::wstring &out)
{
	out.clear();
	if (s.empty()) return true;
	const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
	if (n <= 0) return false;
	out.resize((size_t)n);
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &out[0], n);
	return true;
}

std::string narrow(const std::wstring &w)
{
	if (w.empty()) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	if (n <= 0) return std::string();
	std::string out((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], n, nullptr, nullptr);
	return out;
}

uint32_t linux_errno(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:       return L_ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:      return L_EACCES;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:      return L_EEXIST;
	case ERROR_DIR_NOT_EMPTY:       return L_ENOTEMPTY;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:    return L_ENOSPC;
	case ERROR_WRITE_PROTECT:       return L_EROFS;
	case ERROR_NOT_SAME_DEVICE:     return L_EXDEV;
	case ERROR_FILENAME_EXCED_RANGE: return L_ENAMETOOLONG;
	case ERROR_DIRECTORY:           return L_ENOTDIR;
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_INVALID_PARAMETER:   return L_EINVAL;
	case ERROR_BUSY:                return L_EBUSY;
	default:                        return L_EIO;
	}
}

// Seconds and nanoseconds since the Unix epoch from a FILETIME, which counts
// 100-nanosecond intervals from 1601.
void unix_time(const FILETIME &ft, uint64_t &sec, uint64_t &nsec)
{
	constexpr uint64_t EPOCH_DIFF = 116444736000000000ull;
	const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	if (t < EPOCH_DIFF) { sec = 0; nsec = 0; return; }
	sec = (t - EPOCH_DIFF) / 10000000ull;
	nsec = ((t - EPOCH_DIFF) % 10000000ull) * 100ull;
}

FILETIME file_time(uint64_t sec, uint64_t nsec)
{
	const uint64_t t = sec * 10000000ull + nsec / 100ull + 116444736000000000ull;
	FILETIME ft;
	ft.dwLowDateTime = (DWORD)t;
	ft.dwHighDateTime = (DWORD)(t >> 32);
	return ft;
}

struct HostStat {
	bool dir = false;
	bool readonly = false;
	uint64_t size = 0;
	uint64_t id = 0;         // what the guest sees; see Virtio9p::guest_id
	uint64_t host_id = 0;    // the NTFS file index
	uint64_t atime = 0, atime_ns = 0, mtime = 0, mtime_ns = 0;
	uint64_t ctime = 0, ctime_ns = 0, btime = 0, btime_ns = 0;
};

constexpr DWORD SHARE_ALL = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

bool final_path(HANDLE h, std::wstring &out)
{
	std::vector<wchar_t> buf(32768);
	const DWORD n = GetFinalPathNameByHandleW(h, buf.data(), (DWORD)buf.size(),
	                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (n == 0 || n >= buf.size()) return false;
	out.assign(buf.data(), n);
	for (wchar_t &c : out) c = (wchar_t)towlower(c);
	return true;
}

Qid qid_of(const HostStat &st)
{
	return Qid{(uint8_t)(st.dir ? QTDIR : 0), 0, st.id};
}

} // namespace

// The host side of the server, kept apart from the transport so each piece
// can be read on its own.
class NinePServer {
public:
	explicit NinePServer(Virtio9p &dev) : d(dev) {}

	std::wstring host_path(const std::string &rel)
	{
		std::wstring p = d.root_w;
		if (!rel.empty()) {
			std::wstring w;
			widen(rel, w);
			for (wchar_t &c : w) if (c == L'/') c = L'\\';
			if (p.back() != L'\\') p += L'\\';
			p += w;
		}
		return p;
	}

	// Whether a resolved host path is inside the shared directory. Names
	// are validated before they reach the host, so the only way out is a
	// link or junction inside the folder that points elsewhere -- and the
	// final path, which follows those, is what is checked.
	bool inside(const std::wstring &resolved)
	{
		const std::wstring &rf = d.root_final;
		if (resolved.size() < rf.size() || resolved.compare(0, rf.size(), rf) != 0) return false;
		return resolved.size() == rf.size() || rf.back() == L'\\' || resolved[rf.size()] == L'\\';
	}

	bool stat(const std::string &rel, HostStat &st, uint32_t &err)
	{
		const HANDLE h = CreateFileW(host_path(rel).c_str(), FILE_READ_ATTRIBUTES, SHARE_ALL,
		                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE) { err = linux_errno(GetLastError()); return false; }
		BY_HANDLE_FILE_INFORMATION bi;
		std::wstring resolved;
		const bool ok = GetFileInformationByHandle(h, &bi) && final_path(h, resolved);
		CloseHandle(h);
		if (!ok) { err = L_EIO; return false; }
		if (!inside(resolved)) { err = L_EACCES; return false; }
		st.dir = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		st.readonly = (bi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
		st.size = st.dir ? 0 : (((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow);
		// The NTFS file index is stable across renames, which a path hash
		// would not be, and Linux keys its inode cache on this.
		st.host_id = ((uint64_t)bi.nFileIndexHigh << 32) | bi.nFileIndexLow;
		// But the number the guest sees is not the index itself. NTFS hands
		// indices out according to everything else happening on the volume,
		// so a file the guest creates would get a different inode number on
		// every run. Numbering files in the order the guest first sees them
		// depends only on what the guest has done.
		st.id = d.guest_id(st.host_id);
		unix_time(bi.ftLastWriteTime, st.mtime, st.mtime_ns);
		// Access and change time are reported as the modification time.
		// The host's access time moves whenever anything on the host reads
		// the file, this server included, and NTFS has no change time that
		// means what POSIX's does.
		st.atime = st.ctime = st.mtime;
		st.atime_ns = st.ctime_ns = st.mtime_ns;
		unix_time(bi.ftCreationTime, st.btime, st.btime_ns);
		return true;
	}

	// The guest's clock, for every timestamp the guest causes.
	//
	// A file the guest writes used to get the host's time, from the host's
	// clock, at whatever moment the host got there -- so `ls -l` in the
	// guest, and every build tool that compares timestamps, gave a different
	// answer on every run. The guest's time is the instruction count
	// instead: the device tree's timebase is 1e9, so one instruction is
	// exactly one nanosecond of guest time, counted from a fixed epoch. It
	// is written to the host file too, so a later run that finds the file
	// already there sees the same time this one gave it.
	FILETIME guest_time() const
	{
		constexpr uint64_t VIRTUAL_EPOCH = 1704067200ull;   // 2024-01-01T00:00:00Z
		return file_time(VIRTUAL_EPOCH + d.guest_ns / 1000000000ull, d.guest_ns % 1000000000ull);
	}

	// Stamp a file the guest just changed. Setting a time explicitly through
	// a handle also stops NTFS updating it through that handle later, so a
	// write that NTFS would otherwise timestamp at close keeps this one.
	void touch(HANDLE h, bool created)
	{
		const FILETIME t = guest_time();
		SetFileTime(h, created ? &t : nullptr, &t, &t);
	}

	void touch_path(const std::string &rel, bool created = false)
	{
		const HANDLE h = CreateFileW(host_path(rel).c_str(), FILE_WRITE_ATTRIBUTES, SHARE_ALL, nullptr,
		                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (h == INVALID_HANDLE_VALUE) return;
		touch(h, created);
		CloseHandle(h);
	}

	Virtio9p::Fid *fid(uint32_t n)
	{
		const auto it = d.fids.find(n);
		return it == d.fids.end() ? nullptr : &it->second;
	}

	// Open the file at f.path for I/O. `disposition` is CreateFileW's.
	uint32_t open_file(Virtio9p::Fid &f, uint32_t flags, DWORD disposition, bool *created = nullptr)
	{
		DWORD access = GENERIC_READ;
		const uint32_t acc = flags & L_O_ACCMODE;
		if (acc == L_O_WRONLY) access = GENERIC_WRITE;
		if (acc == L_O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
		if (flags & L_O_TRUNC) access |= GENERIC_WRITE;
		// Shared for everything, delete included, so a Windows program can
		// still open, rename or delete the file while the guest has it open
		// -- which is the point of a shared folder.
		const HANDLE h = CreateFileW(host_path(f.path).c_str(), access, SHARE_ALL, nullptr,
		                             disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return linux_errno(GetLastError());
		// OPEN_ALWAYS and CREATE_ALWAYS report an existing file this way.
		const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
		if (created)
			*created = disposition == CREATE_NEW
			           || ((disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) && !existed);
		std::wstring resolved;
		if (!final_path(h, resolved) || !inside(resolved)) {
			CloseHandle(h);
			return L_EACCES;
		}
		d.close_fid(f);
		f.handle = h;
		f.opened = true;
		f.append = (flags & L_O_APPEND) != 0;
		return 0;
	}

	void getattr_reply(std::vector<uint8_t> &resp, uint16_t tag, const HostStat &st)
	{
		begin_reply(resp, (uint8_t)(Tgetattr + 1), tag);
		Writer w{resp};
		w.u64(0x7FF);                  // P9_STATS_BASIC: everything below is valid
		w.qid(qid_of(st));
		// Windows has no owners or mode bits. Everything is root's and
		// open to all, except that the read-only attribute clears the
		// write bits, so a guest sees that a file cannot be written.
		uint32_t mode = st.dir ? 040777 : 0100666;
		if (st.readonly && !st.dir) mode &= ~0222u;
		w.u32(mode);
		w.u32(0);                      // uid
		w.u32(0);                      // gid
		w.u64(st.dir ? 2 : 1);         // nlink
		w.u64(0);                      // rdev
		w.u64(st.size);
		w.u64(4096);                   // blksize
		w.u64((st.size + 511) / 512);  // blocks
		w.u64(st.atime); w.u64(st.atime_ns);
		w.u64(st.mtime); w.u64(st.mtime_ns);
		w.u64(st.ctime); w.u64(st.ctime_ns);
		w.u64(st.btime); w.u64(st.btime_ns);
		w.u64(0);                      // gen
		w.u64(0);                      // data_version
		finish_reply(resp);
	}

	void rename_fids(const std::string &from, const std::string &to)
	{
		for (auto &kv : d.fids) {
			std::string &p = kv.second.path;
			if (p == from) p = to;
			else if (p.size() > from.size() && p.compare(0, from.size(), from) == 0 && p[from.size()] == '/')
				p = to + p.substr(from.size());
		}
	}

	void handle(const std::vector<uint8_t> &req, std::vector<uint8_t> &resp);

private:
	Virtio9p &d;
};

void NinePServer::handle(const std::vector<uint8_t> &req, std::vector<uint8_t> &resp)
{
	Reader r{req, 0};
	r.u32();                           // size; the buffer's own length is what is trusted
	const uint8_t type = r.u8();
	const uint16_t tag = r.u16();
	if (r.bad) { error_reply(resp, 0xFFFF, L_EINVAL); return; }

	switch (type) {
	case Tversion: {
		const uint32_t client_msize = r.u32();
		const std::string version = r.str();
		d.reset_fids();
		d.msize = std::min<uint32_t>(std::max<uint32_t>(client_msize, 4096), 1u << 20);
		begin_reply(resp, (uint8_t)(Tversion + 1), tag);
		Writer w{resp};
		w.u32(d.msize);
		// Only the Linux dialect is served; anything else is told so, and
		// the client refuses to mount rather than speaking a protocol this
		// server does not implement.
		w.str(version == "9P2000.L" ? "9P2000.L" : "unknown");
		finish_reply(resp);
		return;
	}

	case Tattach: {
		const uint32_t fid_n = r.u32();
		r.u32();                       // afid
		r.str();                       // uname
		r.str();                       // aname
		r.u32();                       // n_uname
		if (r.bad) break;
		HostStat st;
		uint32_t err = 0;
		if (!stat("", st, err)) { error_reply(resp, tag, err); return; }
		if (Virtio9p::Fid *old = fid(fid_n)) d.close_fid(*old);
		d.fids[fid_n] = Virtio9p::Fid{};
		begin_reply(resp, (uint8_t)(Tattach + 1), tag);
		Writer{resp}.qid(qid_of(st));
		finish_reply(resp);
		return;
	}

	case Tflush:
		r.u16();
		begin_reply(resp, (uint8_t)(Tflush + 1), tag);
		finish_reply(resp);
		return;

	case Twalk: {
		const uint32_t fid_n = r.u32();
		const uint32_t newfid_n = r.u32();
		const uint16_t count = r.u16();
		std::vector<std::string> names;
		for (uint16_t i = 0; i < count && !r.bad; i++) names.push_back(r.str());
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		if (newfid_n != fid_n && fid(newfid_n)) { error_reply(resp, tag, L_EBADF); return; }

		std::string cur = f->path;
		std::vector<Qid> qids;
		uint32_t err = 0;
		for (size_t i = 0; i < names.size(); i++) {
			std::string next;
			if (names[i] == "..") next = parent_of(cur);   // clamped at the root
			else if (names[i] == ".") next = cur;
			else if (!valid_name(names[i])) { err = L_ENOENT; break; }
			else next = join(cur, names[i]);
			HostStat st;
			if (!stat(next, st, err)) break;
			if (i + 1 < names.size() && !st.dir) { err = L_ENOTDIR; break; }
			qids.push_back(qid_of(st));
			cur = next;
		}
		if (!names.empty() && qids.empty()) { error_reply(resp, tag, err ? err : L_ENOENT); return; }
		// The new fid exists only if the whole walk succeeded; a partial
		// walk reports how far it got and creates nothing.
		if (qids.size() == names.size()) {
			Virtio9p::Fid nf;
			nf.path = cur;
			if (newfid_n == fid_n) d.close_fid(*f);
			d.fids[newfid_n] = nf;
		}
		begin_reply(resp, (uint8_t)(Twalk + 1), tag);
		Writer w{resp};
		w.u16((uint16_t)qids.size());
		for (const Qid &q : qids) w.qid(q);
		finish_reply(resp);
		return;
	}

	case Tgetattr: {
		const uint32_t fid_n = r.u32();
		r.u64();                       // request mask: everything is always returned
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		HostStat st;
		uint32_t err = 0;
		if (!stat(f->path, st, err)) { error_reply(resp, tag, err); return; }
		getattr_reply(resp, tag, st);
		return;
	}

	case Tsetattr: {
		const uint32_t fid_n = r.u32();
		const uint32_t valid = r.u32();
		const uint32_t mode = r.u32();
		r.u32();                       // uid
		r.u32();                       // gid
		const uint64_t size = r.u64();
		const uint64_t atime_s = r.u64(), atime_ns = r.u64();
		const uint64_t mtime_s = r.u64(), mtime_ns = r.u64();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		HostStat st;
		uint32_t err = 0;
		if (!stat(f->path, st, err)) { error_reply(resp, tag, err); return; }
		const std::wstring path = host_path(f->path);

		if ((valid & P9_SETATTR_MODE) && !st.dir) {
			// The one mode bit Windows can represent: whether the owner
			// may write. chmod -w sets the read-only attribute.
			DWORD attr = GetFileAttributesW(path.c_str());
			if (attr != INVALID_FILE_ATTRIBUTES) {
				if (mode & 0200) attr &= ~(DWORD)FILE_ATTRIBUTE_READONLY;
				else attr |= FILE_ATTRIBUTE_READONLY;
				if (!SetFileAttributesW(path.c_str(), attr)) {
					error_reply(resp, tag, linux_errno(GetLastError()));
					return;
				}
			}
		}
		if (valid & P9_SETATTR_SIZE) {
			if (st.dir) { error_reply(resp, tag, L_EISDIR); return; }
			const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, SHARE_ALL, nullptr,
			                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) { error_reply(resp, tag, linux_errno(GetLastError())); return; }
			LARGE_INTEGER end;
			end.QuadPart = (LONGLONG)size;
			const bool ok = SetFilePointerEx(h, end, nullptr, FILE_BEGIN) && SetEndOfFile(h);
			const DWORD e = GetLastError();
			if (ok) touch(h, false);
			CloseHandle(h);
			if (!ok) { error_reply(resp, tag, linux_errno(e)); return; }
		}
		if (valid & (P9_SETATTR_ATIME | P9_SETATTR_MTIME)) {
			const HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, SHARE_ALL, nullptr,
			                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if (h == INVALID_HANDLE_VALUE) { error_reply(resp, tag, linux_errno(GetLastError())); return; }
			// "Now" for touch(1) and utimensat(UTIME_NOW) is the guest's.
			const FILETIME now = guest_time();
			const FILETIME a = (valid & P9_SETATTR_ATIME_SET) ? file_time(atime_s, atime_ns) : now;
			const FILETIME m = (valid & P9_SETATTR_MTIME_SET) ? file_time(mtime_s, mtime_ns) : now;
			const bool ok = SetFileTime(h, nullptr,
			                            (valid & P9_SETATTR_ATIME) ? &a : nullptr,
			                            (valid & P9_SETATTR_MTIME) ? &m : nullptr);
			const DWORD e = GetLastError();
			CloseHandle(h);
			if (!ok) { error_reply(resp, tag, linux_errno(e)); return; }
		}
		// Owner and group changes succeed without effect: there is nothing
		// on the host to change, and failing them would break cp -p and
		// every package manager that sets them out of habit.
		begin_reply(resp, (uint8_t)(Tsetattr + 1), tag);
		finish_reply(resp);
		return;
	}

	case Tlopen: {
		const uint32_t fid_n = r.u32();
		const uint32_t flags = r.u32();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		HostStat st;
		uint32_t err = 0;
		if (!stat(f->path, st, err)) { error_reply(resp, tag, err); return; }
		if (st.dir) {
			// A directory has nothing to hold open; its listing is taken
			// on the first readdir.
			d.close_fid(*f);
			f->opened = true;
			f->listed = false;
		} else {
			err = open_file(*f, flags, (flags & L_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING);
			if (err) { error_reply(resp, tag, err); return; }
			if (flags & L_O_TRUNC) touch((HANDLE)f->handle, false);
			if (!stat(f->path, st, err)) { error_reply(resp, tag, err); return; }
		}
		begin_reply(resp, (uint8_t)(Tlopen + 1), tag);
		Writer w{resp};
		w.qid(qid_of(st));
		w.u32(0);                      // iounit: let the client size I/O from msize
		finish_reply(resp);
		return;
	}

	case Tlcreate: {
		const uint32_t fid_n = r.u32();
		const std::string name = r.str();
		const uint32_t flags = r.u32();
		r.u32();                       // mode
		r.u32();                       // gid
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		if (!valid_name(name)) { error_reply(resp, tag, L_EINVAL); return; }
		const std::string dir = f->path;
		f->path = join(dir, name);
		DWORD disposition = OPEN_ALWAYS;
		if (flags & L_O_EXCL) disposition = CREATE_NEW;
		else if (flags & L_O_TRUNC) disposition = CREATE_ALWAYS;
		bool created = false;
		uint32_t err = open_file(*f, flags, disposition, &created);
		if (!err) {
			if (created) {
				touch((HANDLE)f->handle, true);
				touch_path(dir);       // a new entry changes its directory
			} else if (flags & L_O_TRUNC) {
				touch((HANDLE)f->handle, false);
			}
		}
		HostStat st;
		if (!err && !stat(f->path, st, err)) {}
		if (err) {
			f->path = dir;             // a failed create leaves the fid on its directory
			error_reply(resp, tag, err);
			return;
		}
		begin_reply(resp, (uint8_t)(Tlcreate + 1), tag);
		Writer w{resp};
		w.qid(qid_of(st));
		w.u32(0);
		finish_reply(resp);
		return;
	}

	case Tread: {
		const uint32_t fid_n = r.u32();
		const uint64_t offset = r.u64();
		uint32_t count = r.u32();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f || !f->opened) { error_reply(resp, tag, L_EBADF); return; }
		if (!f->handle) { error_reply(resp, tag, L_EISDIR); return; }
		count = std::min(count, d.msize - 11);
		begin_reply(resp, (uint8_t)(Tread + 1), tag);
		Writer{resp}.u32(0);
		const size_t data_at = resp.size();
		resp.resize(data_at + count);
		// Positional: the offset goes in the OVERLAPPED structure, so reads
		// on one handle never depend on a shared file pointer.
		OVERLAPPED ov{};
		ov.Offset = (DWORD)offset;
		ov.OffsetHigh = (DWORD)(offset >> 32);
		DWORD got = 0;
		if (count && !ReadFile((HANDLE)f->handle, resp.data() + data_at, count, &got, &ov)) {
			const DWORD e = GetLastError();
			if (e != ERROR_HANDLE_EOF) { error_reply(resp, tag, linux_errno(e)); return; }
			got = 0;
		}
		resp.resize(data_at + got);
		for (int i = 0; i < 4; i++) resp[data_at - 4 + (size_t)i] = (uint8_t)(got >> (8 * i));
		finish_reply(resp);
		return;
	}

	case Twrite: {
		const uint32_t fid_n = r.u32();
		const uint64_t offset = r.u64();
		uint32_t count = r.u32();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f || !f->opened || !f->handle) { error_reply(resp, tag, L_EBADF); return; }
		count = (uint32_t)std::min<size_t>(count, req.size() - r.p);
		OVERLAPPED ov{};
		if (f->append) {
			// All ones means "at the end, whatever that is now", which is
			// O_APPEND's meaning, and it stays right if the host appends too.
			ov.Offset = 0xFFFFFFFF;
			ov.OffsetHigh = 0xFFFFFFFF;
		} else {
			ov.Offset = (DWORD)offset;
			ov.OffsetHigh = (DWORD)(offset >> 32);
		}
		DWORD put = 0;
		if (count && !WriteFile((HANDLE)f->handle, req.data() + r.p, count, &put, &ov)) {
			error_reply(resp, tag, linux_errno(GetLastError()));
			return;
		}
		if (put) touch((HANDLE)f->handle, false);
		begin_reply(resp, (uint8_t)(Twrite + 1), tag);
		Writer{resp}.u32(put);
		finish_reply(resp);
		return;
	}

	case Treaddir: {
		const uint32_t fid_n = r.u32();
		const uint64_t offset = r.u64();
		uint32_t count = r.u32();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f || !f->opened) { error_reply(resp, tag, L_EBADF); return; }
		// The listing is taken once, when the client starts from the top,
		// and served from that snapshot as it pages through. Offsets are
		// indices into it, so a file created mid-listing cannot make an
		// entry appear twice or be skipped.
		if (offset == 0 || !f->listed) {
			f->listing.clear();
			f->listing.push_back({".", true});
			f->listing.push_back({"..", true});
			std::wstring pattern = host_path(f->path);
			if (pattern.back() != L'\\') pattern += L'\\';
			pattern += L'*';
			WIN32_FIND_DATAW fd;
			const HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
			                                  FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
			if (h == INVALID_HANDLE_VALUE) {
				const DWORD e = GetLastError();
				if (e != ERROR_FILE_NOT_FOUND) { error_reply(resp, tag, linux_errno(e)); return; }
			} else {
				do {
					const std::wstring n = fd.cFileName;
					if (n == L"." || n == L"..") continue;
					f->listing.push_back({narrow(n), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			// Sorted, rather than whatever order the host filesystem
			// enumerates in: NTFS happens to sort, but FAT and exFAT list in
			// creation order, which depends on the folder's history.
			std::sort(f->listing.begin() + 2, f->listing.end(),
			          [](const Virtio9p::DirEntry &x, const Virtio9p::DirEntry &y) { return x.name < y.name; });
			f->listed = true;
		}
		count = std::min(count, d.msize - 11);
		begin_reply(resp, (uint8_t)(Treaddir + 1), tag);
		Writer w{resp};
		w.u32(0);
		const size_t data_at = resp.size();
		for (uint64_t i = offset; i < f->listing.size(); i++) {
			const Virtio9p::DirEntry &e = f->listing[(size_t)i];
			const size_t entry_size = 13 + 8 + 1 + 2 + e.name.size();
			if (resp.size() - data_at + entry_size > count) break;
			std::string rel = f->path;
			if (e.name == "..") rel = parent_of(f->path);
			else if (e.name != ".") rel = join(f->path, e.name);
			HostStat st;
			uint32_t err = 0;
			Qid q{(uint8_t)(e.dir ? QTDIR : 0), 0, 0};
			if (stat(rel, st, err)) q = qid_of(st);
			w.qid(q);
			w.u64(i + 1);              // where the next read picks up
			w.u8(e.dir ? DT_DIR : DT_REG);
			w.str(e.name);
		}
		const uint32_t n = (uint32_t)(resp.size() - data_at);
		for (int i = 0; i < 4; i++) resp[data_at - 4 + (size_t)i] = (uint8_t)(n >> (8 * i));
		finish_reply(resp);
		return;
	}

	case Tmkdir: {
		const uint32_t fid_n = r.u32();
		const std::string name = r.str();
		r.u32();                       // mode
		r.u32();                       // gid
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		if (!valid_name(name)) { error_reply(resp, tag, L_EINVAL); return; }
		const std::string rel = join(f->path, name);
		if (!CreateDirectoryW(host_path(rel).c_str(), nullptr)) {
			error_reply(resp, tag, linux_errno(GetLastError()));
			return;
		}
		touch_path(rel, true);
		touch_path(f->path);
		HostStat st;
		uint32_t err = 0;
		if (!stat(rel, st, err)) { error_reply(resp, tag, err); return; }
		begin_reply(resp, (uint8_t)(Tmkdir + 1), tag);
		Writer{resp}.qid(qid_of(st));
		finish_reply(resp);
		return;
	}

	case Tunlinkat: {
		const uint32_t fid_n = r.u32();
		const std::string name = r.str();
		const uint32_t flags = r.u32();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		if (!valid_name(name)) { error_reply(resp, tag, L_ENOENT); return; }
		const std::string rel = join(f->path, name);
		HostStat st;
		uint32_t err = 0;
		if (!stat(rel, st, err)) { error_reply(resp, tag, err); return; }
		const bool want_dir = (flags & AT_REMOVEDIR) != 0;
		if (st.dir && !want_dir) { error_reply(resp, tag, L_EISDIR); return; }
		if (!st.dir && want_dir) { error_reply(resp, tag, L_ENOTDIR); return; }
		const bool ok = st.dir ? RemoveDirectoryW(host_path(rel).c_str())
		                       : DeleteFileW(host_path(rel).c_str());
		if (!ok) { error_reply(resp, tag, linux_errno(GetLastError())); return; }
		// A file created later must not inherit this one's inode number just
		// because NTFS happened to reuse its index.
		d.forget_id(st.host_id);
		touch_path(f->path);
		begin_reply(resp, (uint8_t)(Tunlinkat + 1), tag);
		finish_reply(resp);
		return;
	}

	case Trenameat:
	case Trename: {
		std::string from, to;
		if (type == Trenameat) {
			const uint32_t olddir = r.u32();
			const std::string oldname = r.str();
			const uint32_t newdir = r.u32();
			const std::string newname = r.str();
			if (r.bad) break;
			Virtio9p::Fid *od = fid(olddir), *nd = fid(newdir);
			if (!od || !nd) { error_reply(resp, tag, L_EBADF); return; }
			if (!valid_name(oldname) || !valid_name(newname)) { error_reply(resp, tag, L_EINVAL); return; }
			from = join(od->path, oldname);
			to = join(nd->path, newname);
		} else {
			const uint32_t fid_n = r.u32();
			const uint32_t newdir = r.u32();
			const std::string newname = r.str();
			if (r.bad) break;
			Virtio9p::Fid *f = fid(fid_n), *nd = fid(newdir);
			if (!f || !nd) { error_reply(resp, tag, L_EBADF); return; }
			if (!valid_name(newname) || f->path.empty()) { error_reply(resp, tag, L_EINVAL); return; }
			from = f->path;
			to = join(nd->path, newname);
		}
		// Replacing an existing target, as rename(2) does. MoveFileW alone
		// refuses to, which would break every editor that saves by writing
		// a temporary file and renaming it over the original.
		HostStat moved, replaced;
		uint32_t serr = 0;
		const bool have_moved = stat(from, moved, serr);
		const bool replacing = stat(to, replaced, serr);
		if (!MoveFileExW(host_path(from).c_str(), host_path(to).c_str(), MOVEFILE_REPLACE_EXISTING)) {
			error_reply(resp, tag, linux_errno(GetLastError()));
			return;
		}
		// A target that was replaced is gone, the same as an unlink.
		if (replacing && !(have_moved && replaced.host_id == moved.host_id)) d.forget_id(replaced.host_id);
		touch_path(parent_of(from));
		if (parent_of(to) != parent_of(from)) touch_path(parent_of(to));
		rename_fids(from, to);
		begin_reply(resp, (uint8_t)(type + 1), tag);
		finish_reply(resp);
		return;
	}

	case Tremove: {
		const uint32_t fid_n = r.u32();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		const std::string rel = f->path;
		d.close_fid(*f);
		d.fids.erase(fid_n);           // clunked whether or not the remove works
		HostStat st;
		uint32_t err = 0;
		if (rel.empty()) { error_reply(resp, tag, L_EBUSY); return; }
		if (!stat(rel, st, err)) { error_reply(resp, tag, err); return; }
		const bool ok = st.dir ? RemoveDirectoryW(host_path(rel).c_str())
		                       : DeleteFileW(host_path(rel).c_str());
		if (!ok) { error_reply(resp, tag, linux_errno(GetLastError())); return; }
		d.forget_id(st.host_id);
		touch_path(parent_of(rel));
		begin_reply(resp, (uint8_t)(Tremove + 1), tag);
		finish_reply(resp);
		return;
	}

	case Tclunk: {
		const uint32_t fid_n = r.u32();
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		d.close_fid(*f);
		d.fids.erase(fid_n);
		begin_reply(resp, (uint8_t)(Tclunk + 1), tag);
		finish_reply(resp);
		return;
	}

	case Tstatfs: {
		r.u32();
		if (r.bad) break;
		// Fixed figures, not the host disk's. Free space on the host moves
		// with everything else the host does, so reporting it would make
		// `df` -- and any installer that checks for room first -- differ
		// between runs. A write the host cannot fit still fails, with
		// ENOSPC, at the write.
		constexpr uint64_t BLOCKS = (1ull << 40) / 4096;   // 1 TiB
		begin_reply(resp, (uint8_t)(Tstatfs + 1), tag);
		Writer w{resp};
		w.u32(0x01021997);             // V9FS_MAGIC
		w.u32(4096);
		w.u64(BLOCKS);
		w.u64(BLOCKS);
		w.u64(BLOCKS);
		w.u64(0);                      // files: the host does not say
		w.u64(0);
		w.u64(0);                      // fsid
		w.u32(255);
		finish_reply(resp);
		return;
	}

	case Tfsync: {
		const uint32_t fid_n = r.u32();
		r.u32();                       // datasync
		if (r.bad) break;
		Virtio9p::Fid *f = fid(fid_n);
		if (!f) { error_reply(resp, tag, L_EBADF); return; }
		if (f->handle && !FlushFileBuffers((HANDLE)f->handle)) {
			error_reply(resp, tag, linux_errno(GetLastError()));
			return;
		}
		begin_reply(resp, (uint8_t)(Tfsync + 1), tag);
		finish_reply(resp);
		return;
	}

	case Tlock:
		// Advisory locks are granted: there is one client, and it already
		// arbitrates between its own processes. Refusing them would break
		// programs that lock their files as a matter of course.
		begin_reply(resp, (uint8_t)(Tlock + 1), tag);
		Writer{resp}.u8(0);            // P9_LOCK_SUCCESS
		finish_reply(resp);
		return;

	case Tgetlock: {
		const uint32_t fid_n = r.u32();
		(void)fid_n;
		r.u8();                        // type
		const uint64_t start = r.u64();
		const uint64_t length = r.u64();
		const uint32_t proc_id = r.u32();
		const std::string client = r.str();
		if (r.bad) break;
		begin_reply(resp, (uint8_t)(Tgetlock + 1), tag);
		Writer w{resp};
		w.u8(2);                       // F_UNLCK: nothing else holds it
		w.u64(start);
		w.u64(length);
		w.u32(proc_id);
		w.str(client);
		finish_reply(resp);
		return;
	}

	case Tauth:
	case Tsymlink:
	case Tmknod:
	case Treadlink:
	case Tlink:
	case Txattrwalk:
	case Txattrcreate:
	default:
		// Things a Windows directory cannot represent faithfully. Refused
		// rather than approximated, so a program finds out instead of
		// getting something subtly different from what it asked for.
		error_reply(resp, tag, L_EOPNOTSUPP);
		return;
	}

	// A request whose fields ran past the end of the buffer.
	error_reply(resp, tag, L_EINVAL);
}

bool Virtio9p::open(const std::string &host_dir, const std::string &mount_tag)
{
	std::wstring w;
	if (!widen(host_dir, w)) return false;
	std::vector<wchar_t> buf(32768);
	const DWORD n = GetFullPathNameW(w.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
	if (n == 0 || n >= buf.size()) return false;
	std::wstring full(buf.data(), n);
	while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) full.pop_back();
	const DWORD attr = GetFileAttributesW(full.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;

	// Extended-length paths, so a deep tree is not cut off at MAX_PATH.
	// Names are validated before they are joined on, which matters here:
	// this form turns off the normalisation that would otherwise tidy up
	// a stray separator.
	if (full.compare(0, 2, L"\\\\") == 0) root_w = L"\\\\?\\UNC\\" + full.substr(2);
	else root_w = L"\\\\?\\" + full;

	const HANDLE h = CreateFileW(root_w.c_str(), FILE_READ_ATTRIBUTES, SHARE_ALL, nullptr,
	                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	const bool ok = final_path(h, root_final);
	CloseHandle(h);
	if (!ok) return false;

	root_utf8 = narrow(full);
	tag = mount_tag.substr(0, 64);
	is_open = true;
	return true;
}

void Virtio9p::close_fid(Fid &f)
{
	if (f.handle) CloseHandle((HANDLE)f.handle);
	f.handle = nullptr;
	f.opened = false;
	f.append = false;
	f.listing.clear();
	f.listed = false;
}

void Virtio9p::handle_message(const std::vector<uint8_t> &req, std::vector<uint8_t> &resp)
{
	NinePServer(*this).handle(req, resp);
}

#else // !_WIN32

// The server is written against the Windows API, which is the platform this
// emulator is built for. Elsewhere the slot simply stays empty.
bool Virtio9p::open(const std::string &, const std::string &) { return false; }
void Virtio9p::close_fid(Fid &f) { f = Fid{}; }
void Virtio9p::handle_message(const std::vector<uint8_t> &, std::vector<uint8_t> &resp)
{
	error_reply(resp, 0xFFFF, L_EOPNOTSUPP);
}

#endif

void Virtio9p::reset_fids()
{
	for (auto &kv : fids) close_fid(kv.second);
	fids.clear();
}

Virtio9p::~Virtio9p()
{
	reset_fids();
}

uint8_t Virtio9p::read8(uint64_t offset) const
{
	// struct virtio_9p_config { le16 tag_len; u8 tag[]; } -- read a byte at
	// a time by the driver, the length as a 16-bit load.
	if (offset >= REG_CONFIG && offset < REG_CONFIG + 2 + 64) {
		const uint64_t o = offset - REG_CONFIG;
		if (o < 2) return (uint8_t)(tag.size() >> (8 * o));
		const uint64_t i = o - 2;
		return i < tag.size() ? (uint8_t)tag[(size_t)i] : 0;
	}
	const uint32_t word = read32(offset & ~3ull);
	return (uint8_t)(word >> (8 * (offset & 3)));
}

uint32_t Virtio9p::read32(uint64_t offset) const
{
	switch (offset) {
	case REG_MAGIC:     return MAGIC;
	case REG_VERSION:   return VERSION;
	case REG_DEVICE_ID: return is_open ? DEVICE_ID : 0;
	case REG_VENDOR_ID: return 0x564d4f44;
	// Word 0: VIRTIO_9P_MOUNT_TAG, without which the driver will not read
	// the tag and nothing can mount it. Word 1: VIRTIO_F_VERSION_1.
	case REG_DEVICE_FEAT: return 1u;
	case REG_QUEUE_NUM_MAX:  return queue_sel == 0 ? QUEUE_MAX : 0;
	case REG_QUEUE_READY:    return queue_sel == 0 ? queue_ready : 0;
	case REG_INTERRUPT_STAT: return interrupt_status;
	case REG_STATUS:         return status;
	case REG_CONFIG_GEN:     return 0;
	default:
		if (offset >= REG_CONFIG) {
			uint32_t v = 0;
			for (int i = 0; i < 4; i++) v |= (uint32_t)read8(offset + (uint64_t)i) << (8 * i);
			return v;
		}
		return 0;
	}
}

void Virtio9p::write32(uint64_t offset, uint32_t value, Memory &mem, Aplic &aplic)
{
	switch (offset) {
	case REG_DEVICE_FEAT_SEL: device_feat_sel = value; break;
	case REG_DRIVER_FEAT_SEL:
	case REG_DRIVER_FEAT:     break;
	case REG_QUEUE_SEL:       queue_sel = value; break;
	case REG_QUEUE_NUM:       if (queue_sel == 0) queue_num = value; break;
	case REG_QUEUE_READY:     if (queue_sel == 0) queue_ready = value; break;
	case REG_QUEUE_DESC_LO:   if (queue_sel == 0) desc_addr = (desc_addr & ~0xFFFFFFFFull) | value; break;
	case REG_QUEUE_DESC_HI:   if (queue_sel == 0) desc_addr = (desc_addr & 0xFFFFFFFFull) | ((uint64_t)value << 32); break;
	case REG_QUEUE_AVAIL_LO:  if (queue_sel == 0) avail_addr = (avail_addr & ~0xFFFFFFFFull) | value; break;
	case REG_QUEUE_AVAIL_HI:  if (queue_sel == 0) avail_addr = (avail_addr & 0xFFFFFFFFull) | ((uint64_t)value << 32); break;
	case REG_QUEUE_USED_LO:   if (queue_sel == 0) used_addr = (used_addr & ~0xFFFFFFFFull) | value; break;
	case REG_QUEUE_USED_HI:   if (queue_sel == 0) used_addr = (used_addr & 0xFFFFFFFFull) | ((uint64_t)value << 32); break;
	case REG_QUEUE_NOTIFY:
		if (value == 0 && queue_ready && is_open) process_queue(mem, aplic);
		break;
	case REG_INTERRUPT_ACK:   interrupt_status &= ~value; break;
	case REG_STATUS:
		status = value;
		if (value == 0) {
			// A reset forgets the queue and every fid the driver held.
			queue_num = queue_ready = 0;
			desc_addr = avail_addr = used_addr = 0;
			last_avail = 0;
			interrupt_status = 0;
			reset_fids();
		}
		break;
	default: break;
	}
}

void Virtio9p::process_queue(Memory &mem, Aplic &aplic)
{
	if (!avail_addr || !used_addr || !desc_addr) return;
	// Every request in this notify is served at the instruction that sent it.
	guest_ns = mem.instruction_count();
	const uint32_t qsz = queue_num ? queue_num : QUEUE_MAX;
	const uint16_t avail_idx = mem.read16(avail_addr + 2);
	bool completed = false;

	std::vector<uint8_t> req, resp;
	while (last_avail != avail_idx) {
		const uint16_t slot = (uint16_t)(last_avail % qsz);
		const uint16_t head = mem.read16(avail_addr + 4 + (uint64_t)slot * 2);

		// One request is one chain: device-readable buffers carrying the
		// T-message, then device-writable ones for the reply. For large
		// reads and writes Linux splits these -- a small header buffer and
		// then the caller's own pages -- so the request is gathered from
		// every readable buffer in order, and the reply is laid across the
		// writable ones in order. That handles the plain and zero-copy
		// forms of every message with one piece of code.
		req.clear();
		std::vector<std::pair<uint64_t, uint32_t>> outs;
		uint16_t d = head;
		for (uint32_t guard = 0; guard <= qsz; guard++) {
			const uint64_t da    = desc_addr + (uint64_t)d * DESC_SIZE;
			const uint64_t addr  = mem.read64(da);
			const uint32_t len   = mem.read32(da + 8);
			const uint16_t flags = mem.read16(da + 12);
			const uint16_t next  = mem.read16(da + 14);
			if (flags & DESC_F_WRITE) {
				outs.push_back({addr, len});
			} else {
				const size_t at = req.size();
				req.resize(at + len);
				mem.read_bytes(addr, req.data() + at, len);
			}
			if (!(flags & DESC_F_NEXT)) break;
			d = next;
		}

		handle_message(req, resp);

		size_t written = 0;
		for (const auto &seg : outs) {
			if (written == resp.size()) break;
			const size_t n = std::min<size_t>(seg.second, resp.size() - written);
			mem.write_bytes(seg.first, resp.data() + written, n);
			written += n;
		}

		const uint16_t used_idx = mem.read16(used_addr + 2);
		const uint64_t slot_addr = used_addr + 4 + (uint64_t)(used_idx % qsz) * 8;
		mem.write32(slot_addr, head);
		mem.write32(slot_addr + 4, (uint32_t)written);
		write16_phys(mem, used_addr + 2, (uint16_t)(used_idx + 1));

		last_avail++;
		completed = true;
	}

	if (completed) {
		interrupt_status |= 1;
		aplic.assert_source(IRQ);
	}
}
