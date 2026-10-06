// Snapshots: the whole machine at one step, written to a directory and read
// back into a freshly started emulator, so a boot that takes minutes becomes
// a load.
//
// What is saved is everything a later step can observe, field by field, in
// this one file, so that the list can be checked against the classes in one
// place: every class it reaches into names this struct a friend. What is not
// saved is everything that only remembers an answer -- the TLB, the fetch,
// data and decode caches, the PMP decode, the interrupt and counter keys. A
// restore invalidates each of them instead, exactly as a CSR write would, and
// they refill with the same answers.
//
// The check that the list is complete is the one determinism already gives:
// boot to N and snapshot, run on to N+M; restore and run to N+M. crash.log
// (registers, CSRs, the clock, the step count and the last 4096 instructions)
// and DOOMV_STATEDUMP (RAM and both framebuffers) must be the same from both,
// and the same as a straight run to N+M. A field missed here shows up as a
// difference there, sooner or later.
//
// Disks are copied whole into the snapshot, since the guest's writes after it
// change the image. A restore runs on a fresh working copy of that copy, so
// neither the snapshot nor the image the machine was started with is ever
// written.
//
// Not supported yet: a shared folder with files open (fids hold host file
// handles and directory listings), and input scripts, which run from their
// start. A snapshot is refused rather than taken wrong.
#include "doom_system.hpp"
#include "extensions.hpp"
#include "mmu.hpp"
#include "pmp.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <type_traits>

namespace fs = std::filesystem;

namespace {

constexpr char MAGIC[8] = {'D', 'O', 'O', 'M', 'V', 'S', 'N', 'P'};
// Bumped whenever the layout below changes. A build reads the layout it
// writes, and the ones before it that it still knows: version 1 is the
// single-hart layout, from before a machine could have several harts, and
// reads as a machine of one; version 2 has no network card, 3 no
// real-time clock, 4 no sound card, with two queues to a virtio device
// where there are now four, 5 no GPU, and 6 no GPU 3D state.
constexpr uint32_t VERSION = 8;

class Writer {
public:
	explicit Writer(const fs::path &p) : f(std::fopen(p.string().c_str(), "wb"))
	{
		if (f) std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
	}
	~Writer() { if (f) std::fclose(f); }
	bool ok() const { return f && !failed; }
	void bytes(const void *p, size_t n) { if (f && std::fwrite(p, 1, n, f) != n) failed = true; }
	template <class T> void pod(const T &v)
	{
		static_assert(std::is_trivially_copyable_v<T>, "saved as bytes");
		bytes(&v, sizeof v);
	}
	// A section marker: a reader that has drifted by a byte stops here.
	void mark(uint32_t tag) { pod(tag); }
	bool close() { const bool good = ok() && std::fclose(f) == 0; f = nullptr; return good; }
	void fail() { failed = true; }
private:
	std::FILE *f;
	bool failed = false;
};

class Reader {
public:
	explicit Reader(const fs::path &p) : f(std::fopen(p.string().c_str(), "rb"))
	{
		if (f) std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
	}
	~Reader() { if (f) std::fclose(f); }
	bool ok() const { return f && !failed; }
	void bytes(void *p, size_t n) { if (!f || std::fread(p, 1, n, f) != n) failed = true; }
	template <class T> void pod(T &v)
	{
		static_assert(std::is_trivially_copyable_v<T>, "saved as bytes");
		bytes(&v, sizeof v);
	}
	template <class T> T get() { T v{}; pod(v); return v; }
	void fail() { failed = true; }
	void mark(uint32_t tag)
	{
		if (get<uint32_t>() != tag && !failed) {
			std::cout << "snapshot: section " << std::hex << tag << std::dec << " is not where it should be\n";
			failed = true;
		}
	}
private:
	std::FILE *f;
	bool failed = false;
};

bool page_is_zero(const uint8_t *p)
{
	uint64_t acc = 0;
	for (unsigned i = 0; i < 4096; i += 8) {
		uint64_t w;
		std::memcpy(&w, p + i, 8);
		acc |= w;
	}
	return acc == 0;
}

// The disk slots, in order: the root disk, then the drives.
constexpr int DISK_SLOTS = 1 + Memory::NUM_DRIVES;

std::string disk_name(int slot, const char *suffix)
{
	return (slot == 0 ? std::string("disk") : "drive" + std::to_string(slot - 1)) + suffix;
}

} // namespace

struct SaveState {
	// The fields, in the order they are written. Each save_* has a load_* that
	// reads exactly what it wrote.

	template <class IO> static void registers(IO &io, Registers &r, uint32_t version)
	{
		// Plain data throughout -- the register files, the CSRs, the history
		// ring crash.log prints -- apart from the lock-step log pointer and
		// the vector registers' pointer, which belong to this process and are
		// put back. Before version 8 the vector pointer did not exist: it is
		// the last field, and everything before it is what was saved then.
		static_assert(std::is_trivially_copyable_v<Registers>, "Registers is saved as bytes");
		auto *const log = r.csr_log;
		uint8_t *const vreg = r.vreg;
		const size_t before_vreg = (size_t)((const char *)&r.vreg - (const char *)&r);
		io.bytes(&r, version >= 8 ? sizeof r : before_vreg);
		r.csr_log = log;
		r.vreg = vreg;
		// Wider than the inline registers: the hart's own storage, after.
		if (!r.vectors_inline()) io.bytes(r.vreg, (size_t)32 * Registers::VLEN_BYTES);
	}

	template <class IO> static void core(IO &io, RiscvCore &c)
	{
		io.pod(c.reservation_valid);
		io.pod(c.reservation_addr);
		io.pod(c.wait_request);
	}

	template <class IO> static void timer(IO &io, Timer &t, uint32_t version)
	{
		io.pod(t.mtime);
		if (version == 1) {
			io.pod(t.cmp[0]);
			t.msip[0] = 0;
		} else {
			for (uint64_t &c : t.cmp) io.pod(c);
			for (uint32_t &m : t.msip) io.pod(m);
		}
		io.pod(t.cmp_gen);
	}

	template <class IO> static void imsic(IO &io, Imsic &m)
	{
		io.pod(m.eidelivery);
		io.pod(m.eithreshold);
		io.pod(m.eip);
		io.pod(m.eie);
		io.pod(m.gen);
		if constexpr (std::is_same_v<IO, Reader>) m.refresh();   // `top` follows from the rest
	}

	template <class IO> static void aplic(IO &io, Aplic &a)
	{
		io.pod(a.domaincfg);
		io.pod(a.sourcecfg);
		io.pod(a.target);
	}

	template <class IO> static void uart(IO &io, Uart &u)
	{
		std::lock_guard<std::mutex> lock(u.rx_mutex);
		io.pod(u.rx_ring);
		io.pod(u.rx_head);
		io.pod(u.rx_tail);
		io.pod(u.ier);
		io.pod(u.fcr);
		io.pod(u.lcr);
		io.pod(u.mcr);
		io.pod(u.scr);
	}

	// A device's virtqueues: four since version 5, two before.
	template <class IO> static void queues(IO &io, VirtioMmio::Queue (&q)[VirtioMmio::MAX_QUEUES], uint32_t version)
	{
		const unsigned n = version >= 5 ? VirtioMmio::MAX_QUEUES : 2;
		for (unsigned i = 0; i < n; i++) io.pod(q[i]);
	}

	template <class IO> static void input(IO &io, VirtioInput &v, uint32_t version)
	{
		io.pod(v.status);
		io.pod(v.device_feat_sel);
		io.pod(v.queue_sel);
		io.pod(v.interrupt_status);
		queues(io, v.queues, version);
		io.pod(v.cfg_select);
		io.pod(v.cfg_subsel);
		std::lock_guard<std::mutex> lock(v.event_mutex);
		uint64_t n = v.events.size();
		io.pod(n);
		if constexpr (std::is_same_v<IO, Writer>) {
			for (const auto &e : v.events) io.pod(e);
		} else {
			v.events.clear();
			for (uint64_t i = 0; i < n && io.ok(); i++) v.events.push_back(io.template get<VirtioInput::Event>());
			v.pending = !v.events.empty();
		}
	}

	// The registers only; the image is a separate file.
	template <class IO> static void blk(IO &io, VirtioBlk &b)
	{
		io.pod(b.status);
		io.pod(b.device_feat_sel);
		io.pod(b.driver_feat_sel);
		io.pod(b.driver_feat);
		io.pod(b.queues[0].num);
		io.pod(b.queues[0].ready);
		io.pod(b.interrupt_status);
		io.pod(b.queues[0].desc);
		io.pod(b.queues[0].avail);
		io.pod(b.queues[0].used);
		io.pod(b.queues[0].last_avail);
	}

	// Version 3 on. The frames waiting for the guest are part of the machine;
	// the network behind the card is not, and a restored run starts with no
	// connections open.
	template <class IO> static void net(IO &io, VirtioNet &n, uint32_t version)
	{
		io.pod(n.status);
		io.pod(n.device_feat_sel);
		io.pod(n.driver_feat_sel);
		io.pod(n.driver_feat);
		io.pod(n.queue_sel);
		io.pod(n.interrupt_status);
		queues(io, n.queues, version);
		uint64_t count = n.rx_pending.size();
		io.pod(count);
		if constexpr (std::is_same_v<IO, Writer>) {
			for (const auto &f : n.rx_pending) {
				uint64_t size = f.size();
				io.pod(size);
				io.bytes(f.data(), f.size());
			}
		} else {
			n.rx_pending.clear();
			for (uint64_t i = 0; i < count && io.ok(); i++) {
				const uint64_t size = io.template get<uint64_t>();
				if (size > VirtioNet::MAX_FRAME) { io.fail(); return; }
				std::vector<uint8_t> f(size);
				io.bytes(f.data(), size);
				n.rx_pending.push_back(std::move(f));
			}
		}
	}

	// The clock, start included: a restored machine keeps the time it had,
	// whatever -rtc the restoring run was given. Version 4 on.
	template <class IO> static void rtc(IO &io, Rtc &r)
	{
		io.pod(r.epoch_ns);
		io.pod(r.offset_ns);
		io.pod(r.time_high);
		io.pod(r.set_high);
		io.pod(r.alarm_high);
		io.pod(r.alarm_ns);
		io.pod(r.alarm_running);
		io.pod(r.irq_enabled);
		io.pod(r.irq_pending);
	}

	// The sound card: its streams and the periods it holds. Version 5 on.
	template <class IO> static void snd(IO &io, VirtioSnd &d)
	{
		io.pod(d.status);
		io.pod(d.device_feat_sel);
		io.pod(d.driver_feat_sel);
		io.pod(d.driver_feat);
		io.pod(d.queue_sel);
		io.pod(d.interrupt_status);
		io.pod(d.queues);
		io.pod(d.streams);
		for (std::deque<VirtioSnd::Msg> *list : {&d.tx, &d.rx}) {
			uint64_t count = list->size();
			io.pod(count);
			if constexpr (std::is_same_v<IO, Reader>) list->assign(count, VirtioSnd::Msg{});
			for (VirtioSnd::Msg &m : *list) {
				if (!io.ok()) return;
				io.pod(m.head);
				io.pod(m.status_addr);
				io.pod(m.rx_bytes);
				uint64_t bytes = m.data.size(), bufs = m.buffers.size();
				io.pod(bytes);
				io.pod(bufs);
				if (bytes > (1u << 20) || bufs > VirtioMmio::QUEUE_MAX) { io.fail(); return; }
				m.data.resize(bytes);
				m.buffers.resize(bufs);
				io.bytes(m.data.data(), bytes);
				for (auto &b : m.buffers) { io.pod(b.first); io.pod(b.second); }
			}
		}
	}

	// The GPU: its resources, host copies included, and the scanout.
	// Version 6 on.
	template <class IO> static void gpu(IO &io, VirtioGpu &g, uint32_t version)
	{
		io.pod(g.status);
		io.pod(g.device_feat_sel);
		io.pod(g.driver_feat_sel);
		io.pod(g.driver_feat);
		io.pod(g.queue_sel);
		io.pod(g.interrupt_status);
		io.pod(g.queues);
		io.pod(g.scanout_resource);
		io.pod(g.scanout_x);
		io.pod(g.scanout_y);
		uint64_t count = g.resources.size();
		io.pod(count);
		if constexpr (std::is_same_v<IO, Writer>) {
			for (auto &[id, r] : g.resources) {
				io.pod(id);
				resource(io, r);
			}
		} else {
			g.resources.clear();
			for (uint64_t i = 0; i < count && io.ok(); i++) {
				const uint32_t id = io.template get<uint32_t>();
				resource(io, g.resources[id]);
			}
		}
		if (version < 7) return;
		// 3D: the host GPU's resources, contents included, and the contexts
		// that hold nothing but them -- made again on a restore
		// (VirtioGpu::restore_resources).
		uint64_t n3d = g.virgl_res.size(), nctx = g.virgl_contexts.size();
		io.pod(n3d);
		if constexpr (std::is_same_v<IO, Writer>) {
			for (auto &[id, r] : g.virgl_res) {
				io.pod(id);
				io.pod(r.args);
				uint64_t nb = r.backing.size(), bytes = r.contents.size();
				io.pod(nb);
				for (auto &b : r.backing) { io.pod(b.first); io.pod(b.second); }
				io.pod(bytes);
				io.bytes(r.contents.data(), bytes);
				r.contents.clear();
				r.contents.shrink_to_fit();
			}
		} else {
			g.virgl_res.clear();
			for (uint64_t i = 0; i < n3d && io.ok(); i++) {
				VirtioGpu::VirglRes &r = g.virgl_res[io.template get<uint32_t>()];
				io.pod(r.args);
				const uint64_t nb = io.template get<uint64_t>();
				if (nb > (1u << 24)) { io.fail(); return; }
				r.backing.resize(nb);
				for (auto &b : r.backing) { io.pod(b.first); io.pod(b.second); }
				const uint64_t bytes = io.template get<uint64_t>();
				if (bytes > VirtioGpu::MEMORY_LIMIT) { io.fail(); return; }
				r.contents.resize(bytes);
				io.bytes(r.contents.data(), bytes);
			}
		}
		io.pod(nctx);
		if constexpr (std::is_same_v<IO, Writer>) {
			for (auto &[id, c] : g.virgl_contexts) {
				io.pod(id);
				io.pod(c.init);
				uint64_t nl = c.name.size(), na = c.attached.size();
				io.pod(nl);
				io.bytes(c.name.data(), nl);
				io.pod(na);
				for (uint32_t a : c.attached) io.pod(a);
			}
		} else {
			g.virgl_contexts.clear();
			for (uint64_t i = 0; i < nctx && io.ok(); i++) {
				VirtioGpu::VirglCtx &c = g.virgl_contexts[io.template get<uint32_t>()];
				io.pod(c.init);
				const uint64_t nl = io.template get<uint64_t>();
				if (nl > 64) { io.fail(); return; }
				c.name.resize(nl);
				io.bytes(c.name.data(), nl);
				const uint64_t na = io.template get<uint64_t>();
				if (na > (1u << 20)) { io.fail(); return; }
				c.attached.resize(na);
				for (uint32_t &a : c.attached) io.pod(a);
			}
			g.restore_pending = g.virgl && (!g.virgl_res.empty() || !g.virgl_contexts.empty());
		}
	}
	template <class IO> static void resource(IO &io, VirtioGpu::Resource &r)
	{
		io.pod(r.format);
		io.pod(r.w);
		io.pod(r.h);
		uint64_t bytes = r.pixels.size(), entries = r.backing.size();
		io.pod(bytes);
		io.pod(entries);
		if (bytes > VirtioGpu::MEMORY_LIMIT || entries > (1u << 24)) { io.fail(); return; }
		r.pixels.resize(bytes);
		r.backing.resize(entries);
		io.bytes(r.pixels.data(), bytes);
		for (auto &e : r.backing) { io.pod(e.first); io.pod(e.second); }
	}

	template <class IO> static void share(IO &io, Virtio9p &s)
	{
		io.pod(s.status);
		io.pod(s.device_feat_sel);
		io.pod(s.queue_sel);
		io.pod(s.queues[0].num);
		io.pod(s.queues[0].ready);
		io.pod(s.interrupt_status);
		io.pod(s.queues[0].desc);
		io.pod(s.queues[0].avail);
		io.pod(s.queues[0].used);
		io.pod(s.queues[0].last_avail);
		io.pod(s.msize);
		io.pod(s.guest_ns);
		io.pod(s.next_id);
		uint64_t n = s.ids.size();
		io.pod(n);
		if constexpr (std::is_same_v<IO, Writer>) {
			for (const auto &kv : s.ids) { io.pod(kv.first); io.pod(kv.second); }
		} else {
			s.ids.clear();
			for (uint64_t i = 0; i < n && io.ok(); i++) {
				const uint64_t k = io.template get<uint64_t>();
				s.ids[k] = io.template get<uint64_t>();
			}
		}
	}

	template <class IO> static void memory(IO &io, Memory &m, uint32_t version)
	{
		// RAM, as the pages that are not all zero: a booted machine has
		// touched far less than it was given.
		uint8_t *const ram = m.ram_data_mut();
		const uint64_t pages = Memory::RAM_SPAN / 4096;
		if constexpr (std::is_same_v<IO, Writer>) {
			for (uint64_t p = 0; p < pages; p++) {
				if (page_is_zero(ram + p * 4096)) continue;
				io.pod(p);
				io.bytes(ram + p * 4096, 4096);
			}
			io.pod(~0ull);
		} else {
			std::memset(ram, 0, Memory::RAM_SPAN);
			for (;;) {
				const uint64_t p = io.template get<uint64_t>();
				if (!io.ok() || p == ~0ull) break;
				if (p >= pages) { io.fail(); return; }
				io.bytes(ram + p * 4096, 4096);
			}
		}
		io.mark(0x52414D00);   // "RAM"

		io.bytes(m.fb.data(), m.fb.size());
		io.bytes(m.lfb.data(), m.lfb.size());
		io.pod(m.poweroff);
		io.pod(m.mouse_dx);
		io.pod(m.mouse_dy);
		io.pod(m.mouse_buttons);
		io.pod(m.mouse_clicked);
		io.pod(m.wad_len);
		io.pod(m.key_queue);
		io.pod(m.key_queue_head);
		io.pod(m.key_queue_tail);
		io.pod(m.instr_count);
		io.pod(m.tick_counter);
		io.pod(m.ms_accum);
		io.pod(m.fb_write_count);
		uint64_t fbg = m.fb_gen.load(), lfbg = m.lfb_gen.load();
		io.pod(fbg);
		io.pod(lfbg);
		if constexpr (std::is_same_v<IO, Reader>) { m.fb_gen = fbg; m.lfb_gen = lfbg; }
		io.pod(m.tohost_value);
		io.pod(m.htif_busy);
		io.mark(0x4D454D00);   // "MEM"

		timer(io, m.timer, version);
		for (unsigned h = 0; h < m.harts(); h++) {
			imsic(io, m.imsic_m[h]);
			imsic(io, m.imsic_s[h]);
		}
		aplic(io, m.aplic);
		uart(io, m.uart);
		input(io, m.kbd_dev, version);
		input(io, m.mouse_dev, version);
		blk(io, m.disk);
		for (VirtioBlk &d : m.drives) blk(io, d);
		share(io, m.share);
		if (version >= 3) net(io, m.net, version);
		if (version >= 4) rtc(io, m.rtc);
		if (version >= 5) snd(io, m.snd);
		if (version >= 6) {
			if constexpr (std::is_same_v<IO, Writer>) m.gpu.save_contents(m);
			gpu(io, m.gpu, version);
		}
		io.mark(0x44455600);   // "DEV"
	}

	// A hart's state beyond its registers and core: a wait in progress, its
	// step count, its extensions. Version 2 on.
	template <class IO> static void hart(IO &io, DoomSystem::Hart &h)
	{
		io.pod(h.waiting);
		io.pod(h.wait_kind);
		io.pod(h.wait_remaining);
		io.pod(h.wait_insn);
		io.pod(h.wait_insn_len);
		io.pod(h.steps);
		io.pod(h.ext);
	}

	template <class IO> static void system(IO &io, DoomSystem &s, uint32_t version)
	{
		for (const auto &h : s.harts) {
			registers(io, h->regs, version);
			io.mark(0x52454700);   // "REG"
			core(io, h->core);
			if (version >= 2) hart(io, *h);
		}
		memory(io, s.memory, version);
		io.pod(s.tick_phase);
		io.pod(s.guest_pointer_x);
		io.pod(s.guest_pointer_y);
		io.pod(s.pending_illegal);
		io.pod(s.pending_illegal_tval);
		uint64_t n = s.uart_backlog.size();
		io.pod(n);
		if constexpr (std::is_same_v<IO, Writer>) {
			for (uint8_t b : s.uart_backlog) io.pod(b);
		} else {
			s.uart_backlog.clear();
			for (uint64_t i = 0; i < n && io.ok(); i++) s.uart_backlog.push_back(io.template get<uint8_t>());
		}
		io.mark(0x53595300);   // "SYS"
	}

	// What must match between the run that saved and the one restoring: the
	// machine's shape, which comes from the command line.
	struct Shape {
		uint64_t ram_size, ram_span;
		uint8_t linux_mode;
		ExtensionConfig supported;
		uint8_t disks[DISK_SLOTS];   // attached, and read-only (2) or writable (1)
		uint8_t share;
	};
	static Shape shape(DoomSystem &s)
	{
		Shape sh{};
		sh.ram_size = Memory::RAM_SIZE;
		sh.ram_span = Memory::RAM_SPAN;
		sh.linux_mode = s.linux_mode;
		sh.supported = SupportedExtensions;
		for (int i = 0; i < DISK_SLOTS; i++) {
			VirtioBlk &b = i == 0 ? s.memory.disk : s.memory.drives[i - 1];
			sh.disks[i] = !b.attached() ? 0 : b.ro ? 2 : 1;
		}
		sh.share = s.memory.share.is_open;
		return sh;
	}

	static bool save(DoomSystem &s, const std::string &dir);
	static bool restore(DoomSystem &s, const std::string &dir);
	static bool export_state(DoomSystem &s, const std::string &dir);
};

bool DoomSystem::save_snapshot(const std::string &dir) { return SaveState::save(*this, dir); }
bool DoomSystem::export_state(const std::string &dir) { return SaveState::export_state(*this, dir); }
bool DoomSystem::restore_snapshot(const std::string &dir) { return SaveState::restore(*this, dir); }

bool SaveState::save(DoomSystem &s, const std::string &dir)
{
	Memory &memory = s.memory;
	if (memory.gpu.has_3d_state()) {
		std::cout << "snapshot: not taken -- a program in the guest is using the GPU for OpenGL or Vulkan, "
		             "and what it holds on the host GPU cannot be saved; take it with no such program running"
		          << std::endl;
		return false;
	}
	if (!memory.share.fids.empty()) {
		std::cout << "snapshot: not taken -- the guest has files open on the shared folder, "
		             "and saving those is not supported yet\n";
		return false;
	}
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) { std::cout << "snapshot: cannot create " << dir << ": " << ec.message() << "\n"; return false; }

	// The images first, flushed, so that what is copied is what the guest wrote.
	for (int i = 0; i < DISK_SLOTS; i++) {
		VirtioBlk &b = i == 0 ? memory.disk : memory.drives[i - 1];
		if (!b.attached()) continue;
		std::fflush(b.file);
		fs::copy_file(b.path(), fs::path(dir) / disk_name(i, ".img"), fs::copy_options::overwrite_existing, ec);
		if (ec) { std::cout << "snapshot: cannot copy " << b.path() << ": " << ec.message() << "\n"; return false; }
	}

	// How to start this machine again, for the tools that restore it.
	if (!s.command_line.empty()) {
		std::ofstream cmd(fs::path(dir) / "command.txt");
		for (const std::string &a : s.command_line) cmd << a << '\n';
	}

	Writer w(fs::path(dir) / "state.bin");
	w.bytes(MAGIC, sizeof MAGIC);
	w.pod(VERSION);
	const Shape sh = shape(s);
	w.pod(sh);
	const uint32_t harts = s.hart_count();
	w.pod(harts);
	const uint8_t net = memory.net.is_connected();
	w.pod(net);
	const uint8_t snd = memory.snd.is_enabled();
	w.pod(snd);
	const uint8_t gpu = memory.gpu.is_enabled();
	w.pod(gpu);
	const uint32_t vlen = Registers::VLEN_BITS;
	w.pod(vlen);
	// The extensions are each hart's, and the current hart's are live in
	// Extensions rather than in its Hart.
	s.cur->ext = Extensions;
	system(w, s, VERSION);
	if (!w.close()) { std::cout << "snapshot: cannot write " << dir << "/state.bin\n"; return false; }
	std::cout << "snapshot of step " << memory.instruction_count() << " saved to " << dir << std::endl;
	return true;
}

bool SaveState::restore(DoomSystem &s, const std::string &dir)
{
	const auto started = std::chrono::steady_clock::now();
	Memory &memory = s.memory;
	Reader r(fs::path(dir) / "state.bin");
	char magic[8];
	r.bytes(magic, sizeof magic);
	if (!r.ok() || std::memcmp(magic, MAGIC, sizeof magic) != 0) {
		std::cout << "restore: " << dir << " holds no snapshot\n";
		return false;
	}
	const uint32_t version = r.get<uint32_t>();
	if (version < 1 || version > VERSION) {
		std::cout << "restore: " << dir << " was written by a build with another snapshot layout\n";
		return false;
	}
	const Shape want = r.get<Shape>(), have = shape(s);
	// Field by field: the struct's padding is not part of the answer.
	std::string differ;
	if (want.ram_size != have.ram_size || want.ram_span != have.ram_span) differ += " RAM size";
	if (want.linux_mode != have.linux_mode) differ += " Linux or DOOM";
	if (std::memcmp(&want.supported, &have.supported, sizeof want.supported) != 0) differ += " -march";
	for (int i = 0; i < DISK_SLOTS; i++)
		if (want.disks[i] != have.disks[i]) differ += " " + disk_name(i, "");
	if (want.share != have.share) differ += " shared folder";
	const uint32_t harts = version == 1 ? 1 : r.get<uint32_t>();
	if (harts != s.hart_count()) differ += " -harts";
	const uint8_t net = version >= 3 ? r.get<uint8_t>() : 0;
	if (net != (uint8_t)memory.net.is_connected()) differ += " -net";
	const uint8_t snd = version >= 5 ? r.get<uint8_t>() : 0;
	if (snd != (uint8_t)memory.snd.is_enabled()) differ += " -snd";
	const uint8_t gpu = version >= 6 ? r.get<uint8_t>() : 0;
	if (gpu != (uint8_t)memory.gpu.is_enabled()) differ += " -gpu";
	const uint32_t vlen = version >= 8 ? r.get<uint32_t>() : (uint32_t)Registers::VLEN_DEFAULT;
	if (vlen != (uint32_t)Registers::VLEN_BITS) differ += " -vlen";
	if (!differ.empty()) {
		std::cout << "restore: this machine is not the one the snapshot was taken on -- start it with "
		             "the same -ram, -march, boot files, disks and shared folder. Different:" << differ << "\n";
		return false;
	}

	// Each disk onto a working copy of the snapshot's image.
	for (int i = 0; i < DISK_SLOTS; i++) {
		VirtioBlk &b = i == 0 ? memory.disk : memory.drives[i - 1];
		if (!b.attached()) continue;
		const bool ro = b.ro;
		const fs::path work = fs::path(dir) / disk_name(i, ".work.img");
		std::error_code ec;
		fs::copy_file(fs::path(dir) / disk_name(i, ".img"), work, fs::copy_options::overwrite_existing, ec);
		if (ec || !b.open(work.string(), ro)) {
			std::cout << "restore: cannot set up " << work.string() << (ec ? ": " + ec.message() : "") << "\n";
			return false;
		}
		std::cout << "restore: " << (i == 0 ? "disk" : "drive " + std::to_string(i - 1))
		          << " now runs on " << work.string() << "\n";
	}

	if (version == 1) r.pod(s.harts[0]->ext);
	system(r, s, version);
	if (!r.ok()) { std::cout << "restore: " << dir << "/state.bin is damaged or truncated\n"; return false; }
	// The machine resumes at the start of a round, with hart 0.
	s.cur = s.harts[0].get();
	Extensions = s.cur->ext;
	memory.select_hart(0);

	// Everything that only remembers an answer, forgotten, as a CSR write or a
	// change of extensions would make it be.
	ExtensionsEpoch++;
	for (unsigned h = s.hart_count(); h-- > 0;) {
		mmu_select_hart(h);
		mmu_tlb_flush();
	}
	pmp::invalidate_cache();
	bump_event_gen();
	for (const auto &h : s.harts) {
		h->regs.state_gen++;
		h->irq_key = ~0ull;
		h->counter_key = ~0ull;
	}
	s.irq_key = ~0ull;
	s.counter_key = ~0ull;
	s.count_reservations();
	// A -replay log runs on from here: what it delivered up to the snapshot's
	// step is already in the machine.
	const uint64_t now = memory.instruction_count();
	while (s.replay_pos < s.replay_events.size() && s.replay_events[s.replay_pos].first <= now) s.replay_pos++;
	while (s.replay_net_pos < s.replay_net.size() && s.replay_net[s.replay_net_pos].first <= now) s.replay_net_pos++;
	while (s.replay_snd_pos < s.replay_snd.size() && s.replay_snd[s.replay_snd_pos].at <= now) s.replay_snd_pos++;
	// The time it took is printed for bench.py, which leaves it out of a
	// measurement that starts from a snapshot.
	const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::printf("restored step %llu from %s in %.3f s\n", (unsigned long long)memory.instruction_count(), dir.c_str(), took);
	std::fflush(stdout);
	return true;
}

// -export-state: what another simulator needs to carry on from here. Not a
// snapshot -- no devices, no caches, nothing DoomV-specific -- only what the
// architecture defines: RAM, the registers, and the CSRs a restore program
// can write back. corun.py turns it into an ELF that does that and then
// returns into the guest at this pc and privilege.
bool SaveState::export_state(DoomSystem &s, const std::string &dir)
{
	if (s.hart_count() != 1) {
		std::cout << "-export-state: one hart only\n";
		return false;
	}
	std::error_code ec;
	fs::create_directories(dir, ec);
	Memory &memory = s.memory;
	DoomSystem::Hart &h = *s.harts[0];
	Registers &r = h.regs;
	{
		std::ofstream ram(fs::path(dir) / "ram.bin", std::ios::binary);
		ram.write((const char *)memory.ram_data(), (std::streamsize)Memory::RAM_SPAN);
		if (!ram) { std::cout << "-export-state: cannot write ram.bin\n"; return false; }
	}
	// The CSRs a restore program writes back, by number: the ones this hart
	// implements and software can set. Views of others (sstatus, sie, sip)
	// are left out -- their owners carry them.
	std::vector<uint16_t> csrs = {
		0x105, 0x106, 0x10A, 0x140, 0x141, 0x142, 0x143, 0x180, 0x14D,          // S
		0x302, 0x303, 0x304, 0x305, 0x306, 0x30A, 0x320, 0x340, 0x341, 0x342,   // M
		0x343, 0x344, 0x34A, 0x34B, 0x30C, 0x10C, 0xB00, 0xB02,
	};
	for (uint16_t c = 0x3A0; c <= 0x3AE; c += 2) csrs.push_back(c);              // pmpcfg, RV64
	for (uint16_t c = 0x3B0; c <= 0x3BF; c++) csrs.push_back(c);                 // pmpaddr0-15
	for (uint16_t c = 0x323; c <= 0x33F; c++) csrs.push_back(c);                 // mhpmevent3-31
	if (Extensions.H)
		for (uint16_t c : {0x600, 0x602, 0x603, 0x604, 0x605, 0x606, 0x607, 0x60A, 0x60C, 0x643, 0x645,
		                   0x64A, 0x680, 0x200, 0x204, 0x205, 0x240, 0x241, 0x242, 0x243, 0x244, 0x280, 0x24D})
			csrs.push_back(c);
	std::ofstream j(fs::path(dir) / "state.json");
	char buf[64];
	const auto hex = [&](uint64_t v) { std::snprintf(buf, sizeof buf, "\"0x%llx\"", (unsigned long long)v); return std::string(buf); };
	j << "{\n  \"pc\": " << hex(r.get_pc()) << ",\n  \"priv\": " << (int)r.get_priv()
	  << ",\n  \"virt\": " << (r.get_virt() ? 1 : 0) << ",\n  \"step\": " << memory.instruction_count()
	  << ",\n  \"vlen\": " << Registers::VLEN_BITS << ",\n  \"vector\": " << (Extensions.V ? 1 : 0)
	  << ",\n  \"ram_base\": " << hex(Memory::RAM_BASE) << ",\n  \"ram_size\": " << hex(Memory::RAM_SPAN)
	  << ",\n  \"mtime\": " << hex(memory.get_timer().get_mtime())
	  << ",\n  \"mtimecmp\": " << hex(memory.get_timer().get_mtimecmp())
	  << ",\n  \"mstatus\": " << hex(h.core.read_csr_effective(r, memory, 0x300))
	  << ",\n  \"fcsr\": " << hex(h.core.read_csr_effective(r, memory, 0x003))
	  << ",\n  \"vtype\": " << hex(r.get_vtype()) << ",\n  \"vl\": " << hex(r.get_vl())
	  << ",\n  \"vstart\": " << hex(r.get_vstart())
	  << ",\n  \"vcsr\": " << hex(Extensions.V ? h.core.read_csr_effective(r, memory, 0x00F) : 0)
	  << ",\n  \"x\": [";
	for (int i = 0; i < 32; i++) j << (i ? ", " : "") << hex(r.read_x(i));
	j << "],\n  \"f\": [";
	for (int i = 0; i < 32; i++) {
		const double d = r.read_f(i);
		uint64_t b;
		std::memcpy(&b, &d, 8);
		j << (i ? ", " : "") << hex(b);
	}
	j << "],\n  \"v\": [";
	for (int i = 0; i < 32; i++) {
		j << (i ? ", " : "") << '"';
		const uint8_t *v = r.read_v(i);
		for (int b = 0; b < Registers::VLEN_BYTES; b++) { std::snprintf(buf, sizeof buf, "%02x", v[b]); j << buf; }
		j << '"';
	}
	j << "],\n  \"csrs\": {";
	for (size_t i = 0; i < csrs.size(); i++)
		j << (i ? ", " : "") << "\"" << csrs[i] << "\": " << hex(h.core.read_csr_effective(r, memory, csrs[i]));
	j << "}\n}\n";
	if (!j) { std::cout << "-export-state: cannot write state.json\n"; return false; }
	std::cout << "exported the state at step " << memory.instruction_count() << " to " << dir << std::endl;
	return true;
}
