#include "doom_system.hpp"
#include "fdt_patch.hpp"
#include "event_gen.hpp"
#include "pmp.hpp"
#include "mmu.hpp"
#include "extensions/ext_fp_common.hpp"   // the bit-level FP helpers, for run_fast
#include "extensions.hpp"
#include <iostream>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
#include <sys/stat.h>

DoomSystem::Hart::Hart(Memory &mem, unsigned id, const Hart *first)
	: id(id), decoder(core, regs, mem, first ? &first->decoder : nullptr), ext(Extensions)
{
	regs.write_csr(0xF14, id);   // mhartid
	core.drop_fetch_page_ctx = this;
	core.drop_fetch_page = [](void *ctx, uint64_t vpage) {
		FetchPage &e = static_cast<Hart *>(ctx)->fetch_cache[vpage & (FETCH_CACHE_SIZE - 1)];
		if (e.vpage == vpage) e.vpage = ~0ull;
	};
}

DoomSystem::DoomSystem()
{
	harts.push_back(std::make_unique<Hart>(memory, 0, nullptr));
	cur = harts[0].get();
}

// ---- Harts ---------------------------------------------------------------
//
// A machine of N harts is N of everything a hart has -- registers, the core
// with its reservation and data caches, a decoder, a fetch cache, a TLB, a
// CLINT msip and mtimecmp, a pair of IMSIC files -- over one memory and one
// clock. They run round-robin, one step each per round, hart 0 first: the
// same order the multi-hart Sail driver (tools/verification/simulators/sail/
// multihart) runs its models in, which is what lets the two be lock-stepped.
// The order never depends on the host, so a run of several harts gives the
// same trace every time, as a run of one does.
//
// The clock moves per round as it moves per step with one hart: once every
// INSNS_PER_TICK rounds in which some hart completed a step, and once in
// every round in which every hart is waiting. A WFI or WRS is not finished
// inside its own step as it is with one hart (run_wait): the hart waits a
// round at a time while the others go on (wait_slot).
//
// With one hart none of this runs, and the machine is exactly what it was.

void DoomSystem::set_harts(unsigned n)
{
	cur = nullptr;
	harts.clear();
	for (unsigned h = 0; h < n; h++) harts.push_back(std::make_unique<Hart>(memory, h, h ? harts[0].get() : nullptr));
	memory.set_harts(n);
	mmu_set_harts(n);
	cur = harts[0].get();
	multi = n > 1;
}

// Makes hart h the one the stepping code means. Everything DoomSystem keeps
// for "the hart" -- the interrupt and counter caches, the extensions misa
// leaves it -- goes back into the hart it belonged to and comes out of the
// next one.
void DoomSystem::select_hart(unsigned h)
{
	Hart *const next = harts[h].get();
	if (next == cur) return;
	cur->ext = Extensions;
	cur->irq_key = irq_key;
	cur->irq_deadline = irq_deadline;
	cur->counter_key = counter_key;
	cur->counts_instret = counts_instret;
	cur->counts_cycle = counts_cycle;
	cur = next;
	irq_key = cur->irq_key;
	irq_deadline = cur->irq_deadline;
	counter_key = cur->counter_key;
	counts_instret = cur->counts_instret;
	counts_cycle = cur->counts_cycle;
	if (std::memcmp(&Extensions, &cur->ext, sizeof(ExtensionConfig)) != 0) {
		Extensions = cur->ext;
		ExtensionsEpoch++;
		bump_event_gen();
	}
	memory.select_hart(h);
	mmu_select_hart(h);
}

// DOOMV_WAITSKIP=0 turns run_round's shortcut for waiting harts off, to
// check that a run is the same without it.
static bool waitskip_enabled()
{
	static const bool on = [] { const char *e = std::getenv("DOOMV_WAITSKIP"); return !(e && e[0] == '0'); }();
	return on;
}

void DoomSystem::run_round()
{
	const bool skip = waitskip_enabled() && !tracing;
	bool retired = false, all_waiting = true;
	const uint64_t now = memory.get_timer().get_mtime();
	for (unsigned h = 0; h < harts.size(); h++) {
		if (debugger.halted) return;
		// A hart in WFI that cannot have been woken since it last looked --
		// no event anywhere (a CSR, CLINT or IMSIC write bumps EventGen) and
		// no timer of its own come due -- spends this round as wait_slot
		// would spend it, without being selected. Most of a many-hart
		// machine's rounds are such harts. Not while tracing: a traced
		// slot also checks the reference.
		Hart &w = *harts[h];
		if (skip && w.waiting && w.wait_kind == RiscvCore::Wait::Wfi && w.wait_remaining > 0
		    && w.wait_event == EventGen && now < w.wait_until) {
			w.wait_remaining--;
			w.retired = false;
			all_waiting = all_waiting && w.wait_remaining > 0;
			continue;
		}
		select_hart(h);
		cur->retired = false;
		hart_slot();
		retired = retired || cur->retired;
		all_waiting = all_waiting && cur->waiting && cur->wait_remaining > 0;
	}
	if (retired) tick_phase++;
	if (tick_phase == INSNS_PER_TICK) {
		tick_phase = 0;
		tick_all_harts();
	} else if (all_waiting) {
		tick_all_harts();
	}
}

// One hart's step in a round. While any other hart holds a reservation, the
// stores this one makes are watched, so that they can end it.
//
// `reserved` counts the harts holding a reservation, so that asking whether
// any other hart does costs the same at 256 harts as at 2. A hart's own
// reservation changes only in its own step, and another's only in
// stores_seen, which keeps the count as it goes.
void DoomSystem::hart_slot()
{
	const bool had = cur->core.reservation_held();
	if (tracing) {
		traced_step();   // traced_step watches its own stores
	} else if (!others_reserved()) {
		step();
	} else {
		std::vector<std::pair<uint64_t, uint8_t>> stored;
		memory.store_log = &stored;
		step();
		memory.store_log = nullptr;
		stores_seen(stored);
	}
	const bool has = cur->core.reservation_held();
	if (has != had) reserved += has ? 1 : -1;
}

bool DoomSystem::others_reserved() const
{
	return reserved > (cur->core.reservation_held() ? 1u : 0u);
}

void DoomSystem::stores_seen(const std::vector<std::pair<uint64_t, uint8_t>> &stored)
{
	if (!others_reserved()) return;
	for (const auto &h : harts) {
		if (h.get() == cur || !h->core.reservation_held()) continue;
		for (const auto &b : stored) h->core.store_by_other_hart(b.first, 1);
		if (!h->core.reservation_held()) reserved--;
	}
}

void DoomSystem::count_reservations()
{
	reserved = 0;
	for (const auto &h : harts) reserved += h->core.reservation_held() ? 1 : 0;
}

static bool filtered_here(const Registers &regs, uint16_t cfg);

// A tick of the clock for the whole machine: each hart's mcycle, where its
// own mcountinhibit and Smcntrpmf filter let it count, and mtime once.
void DoomSystem::tick_all_harts()
{
	for (const auto &h : harts) {
		Registers &r = h->regs;
		if (!(r.read_csr(0x320) & 1) && !filtered_here(r, 0x321)) r.bump_csr(0xB00);
	}
	memory.tick_clock();
}

// A WFI or WRS, with more than one hart: the step that met it ends here, and
// the hart waits from its next slot on. Nothing is recorded for the step
// yet; the instruction completes, or traps, in the slot that ends the wait.
void DoomSystem::begin_wait(uint32_t insn, uint8_t len)
{
	RiscvCore &core = cur->core;
	cur->wait_kind = core.wait_request;
	core.wait_request = RiscvCore::Wait::None;
	cur->waiting = true;
	cur->wait_remaining = MAX_WAIT_TICKS;
	cur->wait_insn = insn;
	cur->wait_insn_len = len;
	cur->wait_event = ~0ull;   // nothing known yet; the first round looks
	step_committed = false;
	step_decoded = false;
}

// One round of a wait: one pass of run_wait's loop, and one try_step of a
// waiting Sail model. It ends on an interrupt pending and enabled, on a WRS
// with no reservation, or after MAX_WAIT_TICKS further rounds; then the
// instruction completes -- or traps, for a WFI below M or a wrs.nto that
// timed out -- as the step it is.
void DoomSystem::wait_slot()
{
	Registers &regs = cur->regs;
	RiscvCore &core = cur->core;
	constexpr uint64_t TW = 1ull << 21, VTW = 1ull << 21;
	const RiscvCore::Wait kind = cur->wait_kind;
	const bool timed_out = cur->wait_remaining == 0;
	int trap = 0;   // 2 illegal, 22 virtual instruction
	bool done = false;
	if (core.wake_for_interrupt(regs, memory)) {
		done = true;
	} else if (kind != RiscvCore::Wait::Wfi && !core.reservation_held()) {
		done = true;
	} else if (timed_out) {
		const PrivMode p = regs.get_priv();
		const bool v = Extensions.H && regs.get_virt();
		const bool tw = (regs.read_csr(0x300) & TW) != 0;
		const bool vtw = v && (regs.read_csr(0x600) & VTW) != 0;
		if (kind == RiscvCore::Wait::Wfi) {
			if (p == PrivMode::S && v) trap = vtw ? 22 : 0;
			else if (p != PrivMode::M) trap = tw ? 2 : 0;
		} else if (kind == RiscvCore::Wait::WrsNto && p != PrivMode::M) {
			trap = tw ? 2 : (vtw ? 22 : 0);
		}
		done = true;
	}
	if (!done) {
		cur->wait_remaining--;
		// What run_round's shortcut checks: nothing has happened since, and
		// mtime is short of every compare value above it.
		cur->wait_event = EventGen;
		const uint64_t now = memory.get_timer().get_mtime();
		uint64_t until = ~0ull;
		const auto consider = [&](uint64_t at) { if (at > now && at < until) until = at; };
		consider(memory.get_timer().get_mtimecmp());
		consider(regs.read_csr(0x14D));                       // stimecmp
		if (regs.read_csr(0x605) == 0) consider(regs.read_csr(0x24D));   // vstimecmp
		else until = now + 1;                                 // with an htimedelta, look every tick
		cur->wait_until = until;
		return;
	}
	cur->waiting = false;
	cur->wait_remaining = 0;
	step_insn = cur->wait_insn;
	step_insn_len = cur->wait_insn_len;
	step_decoded = true;
	const uint64_t pc = regs.get_pc();
	if (trap == 2) core.raise_illegal_instruction(regs, step_insn);
	else if (trap == 22) core.raise_virtual_instruction(regs, step_insn);
	else regs.set_pc(pc + step_insn_len);
	step_committed = trap == 0;
	regs.record_history(pc, step_insn);
	memory.step_instructions(1);
}

bool DoomSystem::attach_disk(const std::string &path)
{
	return memory.get_disk().open(path, /*read_only=*/false);
}

// A path in a form two spellings of the same file agree on, for spotting
// that a drive image is also the root disk. Best effort: if the file
// cannot be resolved, the path as given is compared instead.
static std::string full_path(const std::string &path)
{
#ifdef _WIN32
	char buf[4096];
	if (_fullpath(buf, path.c_str(), sizeof(buf))) {
		std::string s(buf);
		for (char &c : s) c = (char)std::tolower((unsigned char)c);
		return s;
	}
#else
	char buf[PATH_MAX];
	if (realpath(path.c_str(), buf)) return std::string(buf);
#endif
	return path;
}

void DoomSystem::attach_shared(const std::string &dir)
{
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	closedir(d);
	Virtio9p &share = memory.get_share();
	if (share.open(dir, "shared"))
		std::cout << "shared: " << share.host_root() << " -> mount tag \"shared\"\n" << std::flush;
	else
		std::cout << "shared: cannot serve " << dir << ", leaving the slot empty\n" << std::flush;
}

void DoomSystem::attach_drives(const std::string &dir, const std::string &skip)
{
	// opendir rather than std::filesystem: this compiler's MinGW runtime
	// predates a usable <filesystem>, and dirent does the one thing needed.
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	std::vector<std::string> names;
	while (dirent *e = readdir(d)) {
		const std::string name = e->d_name;
		// Raw images only, by extension. Anything else in the folder -- the
		// README, a half-copied file under another name -- is left alone.
		if (name.size() > 4 && name.compare(name.size() - 4, 4, ".img") == 0)
			names.push_back(name);
	}
	closedir(d);
	// Name order, so a drive keeps its device name from one boot to the
	// next: Linux numbers virtio disks in probe order, which is slot order.
	std::sort(names.begin(), names.end());

	const std::string skip_full = skip.empty() ? std::string() : full_path(skip);
	int slot = 0;
	for (const std::string &name : names) {
		const std::string path = dir + "/" + name;
		if (!skip_full.empty() && full_path(path) == skip_full) {
			std::cout << "drives: " << name << " is the root disk, not attaching it twice\n";
			continue;
		}
		if (slot == Memory::NUM_DRIVES) {
			std::cout << "drives: only " << Memory::NUM_DRIVES
			          << " slots; ignoring " << name << " and anything after it\n";
			break;
		}
		VirtioBlk &drive = memory.get_drive(slot);
		if (!drive.open(path, /*read_only=*/false)) {
			std::cout << "drives: cannot open " << path << ", skipping it\n";
			continue;
		}
		std::cout << "drives: " << name << " -> slot " << slot << ", "
		          << (drive.capacity_sectors() * VirtioBlk::SECTOR) / (1024 * 1024) << " MiB"
		          << (drive.read_only() ? ", read-only" : "") << "\n";
		slot++;
	}
	std::cout << std::flush;
}

bool DoomSystem::init(const char *wad_path, const char *elf_path)
{
	if (!headless && !gui.init()) return false;

	controls.load("controls.json");

	std::ifstream wad_file(wad_path, std::ios::binary | std::ios::ate);
	if (!wad_file.is_open()) return false;
	size_t wad_len = (size_t)wad_file.tellg();
	wad_file.seekg(0);
	std::vector<uint8_t> wad_bytes(wad_len);
	wad_file.read((char *)wad_bytes.data(), wad_len);
	if (!memory.load_wad(wad_bytes.data(), wad_len)) return false;

	if (!memory.load_elf(elf_path)) return false;

	// Every hart starts at _start (see riscv.lds) with a0 = its hart id, the
	// boot convention Sail's init_boot_requirements also sets up.
	for (const auto &h : harts) {
		h->regs.set_pc(Memory::RAM_BASE);
		if (h->id) h->regs.write_x(10, h->id);
	}

	return true;
}

bool DoomSystem::init_linux_boot(const char *sbi_path, const char *kernel_path, const char *dtb_path, const char *initrd_path)
{
	if (!headless && !gui.init()) return false;
	linux_mode = true;

	// fw_jump.elf's own build-time FW_TEXT_START already is RAM_BASE (see
	// tools/linux/opensbi/build.sh) -- load_elf places it there unmodified, same
	// as Doom's own guest ELF above.
	if (!memory.load_elf(sbi_path)) return false;

	// Offsets match what fw_jump.elf was built expecting: FW_JUMP_ADDR =
	// FW_TEXT_START + KERNEL_OFFSET, FW_JUMP_FDT_ADDR = FW_TEXT_START +
	// DTB_OFFSET (scripts/build_linux.sh). The initrd offset is DoomV's own
	// choice, not something fw_jump.elf cares about -- it just needs to sit
	// past the DTB with headroom and match the DTB's own linux,initrd-start
	// (scripts/prepare_dtb.py).
	//
	// The kernel sits 128MB in rather than OpenSBI's default 2MB because
	// OpenSBI is built for 4096 harts, and keeps a stack, a scratch area and
	// a share of its heap -- about 18KB -- for each hart the device tree
	// names, straight after its own image: 72MB for 4096. 2MB ran out at
	// about 140 harts.
	//
	// The device tree is 64MB past the kernel. It was 32MB, and the kernel
	// outgrew that when the sound driver went in: its BSS, which it clears
	// before it reads the device tree, ran 200KB into it, and the boot
	// stopped with no output at all.
	constexpr uint64_t KERNEL_OFFSET = 0x8000000, DTB_OFFSET = 0xC000000, INITRD_OFFSET = 0xC100000;
	if (!memory.load_blob(kernel_path, Memory::RAM_BASE + KERNEL_OFFSET)) return false;
	// So that cannot happen quietly again: a RISC-V Image header (magic
	// "RSC\x05" at byte 56) gives the kernel's whole size in memory, BSS
	// included, at byte 16.
	{
		const uint8_t *image = memory.ram_data() + KERNEL_OFFSET;
		uint64_t size = 0;
		std::memcpy(&size, image + 16, sizeof size);
		if (std::memcmp(image + 56, "RSC\x05", 4) == 0 && KERNEL_OFFSET + size > DTB_OFFSET) {
			std::cerr << kernel_path << " needs " << (size >> 20) << " MB in memory, and the device tree is "
			          << ((DTB_OFFSET - KERNEL_OFFSET) >> 20) << " MB after it: move DTB_OFFSET and "
			          << "INITRD_OFFSET in src/doom_system.cpp, with what follows them\n";
			return false;
		}
	}
	if (!memory.load_blob(dtb_path, Memory::RAM_BASE + DTB_OFFSET)) return false;
	// The device tree ships with a memory size baked in; -ram= makes that a
	// choice, so the blob is corrected to match what was actually allocated.
	// A guest told the wrong size does not fail in a way that names memory --
	// too large and it writes past the end, too small and it OOM-kills init.
	{
		uint8_t *dtb = memory.ram_data_mut() + DTB_OFFSET;
		const size_t room = (size_t)(Memory::RAM_SPAN - DTB_OFFSET);
		if (!fdt_set_memory_size(dtb, room, Memory::RAM_BASE, Memory::RAM_SIZE)) {
			std::cerr << "warning: could not set the memory size in " << dtb_path
			          << "; the guest will use the size the file was built with\n";
		}
		// With a GPU, it is the display: the simple-framebuffer goes, or
		// Linux would bind both and put the console on the one that is not
		// shown.
		if (memory.get_gpu().is_enabled() && !fdt_remove_node(dtb, room, "framebuffer@50000000")) {
			std::cerr << "warning: no framebuffer@50000000 in " << dtb_path
			          << " to take out; with -gpu the guest may draw to both\n";
		}
	}
	// Optional now: with a virtio disk attached the kernel mounts a real
	// root filesystem instead, and there is no initramfs to place.
	if (initrd_path && initrd_path[0]
	    && !memory.load_blob(initrd_path, Memory::RAM_BASE + INITRD_OFFSET))
		return false;

	// Every hart enters the firmware together, as the SBI boot protocol has
	// it; OpenSBI picks one to boot on and parks the rest.
	for (const auto &h : harts) {
		h->regs.set_pc(Memory::RAM_BASE);
		h->regs.write_x(10, h->id);                            // a0: hart id
		h->regs.write_x(11, Memory::RAM_BASE + DTB_OFFSET);     // a1: DTB pointer, SBI/Linux boot convention
	}

	return true;
}

uint8_t DoomSystem::translate_key(uint32_t sdl_keysym) const
{
	uint8_t mapped = controls.translate(sdl_keysym);
	if (mapped != 0) return mapped;

	// Numeric values match doomkeys.h exactly (KEY_RIGHTARROW etc) -- see
	// tools/doom/doombuild/doomgeneric/doomgeneric/doomkeys.h. Arrow keys stay
	// here too (not in controls.json) so they keep working alongside WASD.
	switch (sdl_keysym) {
	case SDLK_RIGHT:     return 0xae;
	case SDLK_LEFT:      return 0xac;
	case SDLK_UP:        return 0xad;
	case SDLK_DOWN:      return 0xaf;
	case SDLK_RETURN:    return 13;
	case SDLK_ESCAPE:    return 27;
	case SDLK_TAB:       return 9;
	case SDLK_BACKSPACE: return 0x7f;
	case SDLK_PAUSE:     return 0xff;
	case SDLK_EQUALS:    return 0x3d;
	case SDLK_MINUS:     return 0x2d;
	case SDLK_LCTRL:
	case SDLK_RCTRL:     return 0xa3; // KEY_FIRE
	case SDLK_SPACE:     return 0xa2; // KEY_USE
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:    return 0x80 + 0x36;
	case SDLK_LALT:
	case SDLK_RALT:      return 0x80 + 0x38;
	case SDLK_CAPSLOCK:  return 0x80 + 0x3a;
	case SDLK_HOME:      return 0x80 + 0x47;
	case SDLK_END:       return 0x80 + 0x4f;
	case SDLK_PAGEUP:    return 0x80 + 0x49;
	case SDLK_PAGEDOWN:  return 0x80 + 0x51;
	case SDLK_INSERT:    return 0x80 + 0x52;
	case SDLK_DELETE:    return 0x80 + 0x53;
	case SDLK_F1:  return 0x80 + 0x3b;
	case SDLK_F2:  return 0x80 + 0x3c;
	case SDLK_F3:  return 0x80 + 0x3d;
	case SDLK_F4:  return 0x80 + 0x3e;
	case SDLK_F5:  return 0x80 + 0x3f;
	case SDLK_F6:  return 0x80 + 0x40;
	case SDLK_F7:  return 0x80 + 0x41;
	case SDLK_F8:  return 0x80 + 0x42;
	case SDLK_F9:  return 0x80 + 0x43;
	case SDLK_F10: return 0x80 + 0x44;
	case SDLK_F11: return 0x80 + 0x57;
	case SDLK_F12: return 0x80 + 0x58;
	default:
		// Regular keys: SDL keysyms for a-z/0-9/punctuation are already
		// their ASCII value, which is what Doom expects for non-special keys.
		if (sdl_keysym >= 0x20 && sdl_keysym < 0x7f) return (uint8_t)sdl_keysym;
		return 0;
	}
}

// Smcntrpmf: whether mcyclecfg or minstretcfg stops its counter in the mode
// the hart is in. Sail's counter_priv_filter_bit.
static bool filtered_here(const Registers &regs, uint16_t cfg)
{
	const bool virt = Extensions.H && regs.get_virt();
	int bit;
	switch (regs.get_priv()) {
	case PrivMode::M: bit = 62; break;
	case PrivMode::S: bit = virt ? 59 : 61; break;
	default:          bit = virt ? 58 : 60; break;
	}
	return ((regs.read_csr(cfg) >> bit) & 1) != 0;
}

// Called only when the key has changed; the comparison itself is inline at
// the call sites, since it runs on every step.
void DoomSystem::refresh_counter_enables()
{
	Registers &regs = cur->regs;
	counter_key = regs.keyed(regs.state_gen + ExtensionsEpoch);
	counts_instret = !(regs.read_csr(0x320) & 4) && !filtered_here(regs, 0x322);
	counts_cycle = !(regs.read_csr(0x320) & 1) && !filtered_here(regs, 0x321);
}

// Interrupt checks are skipped while nothing that decides them has changed.
//
// check_and_take_interrupt is a function of the CSRs (mip's software-set
// bits, mie, mideleg, hideleg, mstatus, vsstatus, hvip, hgeie, hgeip,
// hstatus, menvcfg, henvcfg, stimecmp, vstimecmp, htimedelta), the privilege
// and V, the IMSIC files, mtimecmp, the enabled extensions -- and mtime. Each
// of the others bumps a generation when it changes, and mtime only grows, so
// a check that found nothing stays true until one of those generations moves
// or mtime reaches the next compare value above it. Until then the check
// would come out the same, and taking the interrupt at the same step as
// before is exactly what lock-step needs.
bool DoomSystem::interrupt_may_be_due()
{
	Registers &regs = cur->regs;
	Timer &timer = memory.get_timer();
	const uint64_t key = EventGen;
	const uint64_t now = timer.get_mtime();
	if (key == irq_key && now < irq_deadline) return false;

	uint64_t next = ~0ull;
	const auto consider = [&](uint64_t at) { if (at > now && at < next) next = at; };
	consider(timer.get_mtimecmp());
	consider(regs.read_csr(0x14D));          // stimecmp
	// vstimecmp is compared with mtime plus htimedelta, which can wrap; with
	// an offset in play, look again at every tick rather than solve for it.
	if (regs.read_csr(0x605) == 0) consider(regs.read_csr(0x24D));
	else consider(now + 1);
	irq_key = key;
	irq_deadline = next;
	return true;
}

// Prototype switch, so one binary can run both loops: DOOMV_FAST=0 disables.
static bool fast_enabled()
{
	static const bool on = [] { const char *e = std::getenv("DOOMV_FAST"); return !(e && e[0] == '0'); }();
	return on;
}

// Exactly what n calls to step() do, faster.
//
// A run of "simple" steps -- an I/C/M instruction that is not a CSR or system
// op, fetched from a cached code page, whose load or store (if any) hits the
// data caches -- can change the x registers, pc and plain RAM, and nothing
// else. In particular it cannot change anything the per-step checks depend on:
// no CSR write, no privilege change, no TLB flush, no device register, so
// EventGen, state_gen, the TLB generation and ExtensionsEpoch all stay put, and
// so do the fetch and data cache keys, the counter enables and the interrupt
// decision -- except for mtime, which the steps themselves advance. So the
// checks are made once at the start of a run, mtime's deadline is turned into
// a step count, and the bookkeeping every step does to things no simple step
// can observe (the step count, minstret, mtime, mcycle) is added up and
// applied once at the end of the run. The instruction history is still written
// per step.
//
// Anything else -- a cache miss, a device access, a CSR, a trap -- ends the run
// before that step has changed anything, and the step is taken by step(), the
// definition of what a step does. Then a new run starts.
// Prototype statistics: why runs end. Printed at -stopat with DOOMV_FASTSTATS.
static uint64_t fs_fast, fs_slow_entry, fs_deadline, fs_fetch, fs_decode, fs_op[256], fs_ld, fs_st, fs_budget;
static uint64_t fs_ext[64];
// Which run-entry check refused: debug/logs/config, epoch, wait, counters, events, deadline.
static uint64_t fs_entry[6];
void DoomSystem::run_fast(uint64_t n)
{
	Registers &regs = cur->regs;
	RiscvCore &core = cur->core;
	Decoder &decoder = cur->decoder;
	FetchPage *const fetch_cache = cur->fetch_cache;
	Timer &timer = memory.get_timer();
	auto &dcache = decoder.cache;
	auto &ftab = decoder.fast;
	while (n > 0 && !debugger.halted) {
		// Conditions under which no step can be simple. step() for all of them.
		if (debugger.may_halt() || memory.tohost_addr || core.access_log || memory.store_log
		    || decoder.cache_epoch != ExtensionsEpoch
		    || !Extensions.C || !Extensions.XLEN64 || Extensions.ZICFILP
		    || core.wait_request != RiscvCore::Wait::None
		    || regs.keyed(regs.state_gen + ExtensionsEpoch) != counter_key
		    || EventGen != irq_key || timer.get_mtime() >= irq_deadline) {
			fs_slow_entry++;
			// Counted only here, on the way to the slow step.
			fs_entry[(debugger.may_halt() || memory.tohost_addr || core.access_log || memory.store_log
			          || !Extensions.C || !Extensions.XLEN64 || Extensions.ZICFILP) ? 0
			         : decoder.cache_epoch != ExtensionsEpoch ? 1
			         : core.wait_request != RiscvCore::Wait::None ? 2
			         : regs.keyed(regs.state_gen + ExtensionsEpoch) != counter_key ? 3
			         : EventGen != irq_key ? 4 : 5]++;
			step();
			n--;
			continue;
		}
		// How many steps before mtime reaches the interrupt deadline: step j
		// of this run sees mtime + (tick_phase + j) / 2, which must stay below it.
		uint64_t limit = n;
		if (irq_deadline != ~0ull) {
			const uint64_t d = irq_deadline - timer.get_mtime();
			const uint64_t steps = (d > (~0ull >> 2)) ? ~0ull : 2 * d - tick_phase;
			if (steps < limit) limit = steps;
		}
		const uint64_t key = regs.fetch_key(regs.state_gen + mmu_tlb_generation() + ExtensionsEpoch), dkey = regs.data_key(regs.state_gen + mmu_tlb_generation() + ExtensionsEpoch);
		// F and D run here only while mstatus.FS is already Dirty, outside a
		// guest. Then the unit is on, so nothing but a load or store can trap,
		// and the write that marks FS Dirty stores the value already there,
		// which changes nothing a run depends on (Registers::write_csr). Only
		// a CSR instruction, never run here, can change FS, so it holds for the run.
		const bool fp_ok = (regs.read_csr(0x300) & (3ull << 13)) == (3ull << 13) && !regs.get_virt();
		regs.minstret_increment = counts_instret;

		uint64_t pc = regs.get_pc();
		uint64_t done = 0;
		uint32_t last_insn = 0;
		uint8_t last_len = 4;
		uint64_t code_vpage = ~0ull;
		// The history ring's index, held here for the run and stored once at the
		// end: as a member it was loaded and stored every step, 11% of a Ubuntu
		// boot's profile.
		Registers::HistoryRecord *const hist = regs.history_base();
		unsigned hp = (unsigned)regs.history_index();
		const uint8_t *code = nullptr;
		while (done < limit) {
			// Fetch: the code page stays valid for the whole run.
			const uint64_t vpage = pc >> 12;
			const unsigned off = (unsigned)(pc & 0xFFF);
			if (vpage != code_vpage) {
				const FetchPage &e = fetch_cache[vpage & (FETCH_CACHE_SIZE - 1)];
				if (!(e.vpage == vpage && e.key == key)) { fs_fetch++; break; }
				code_vpage = vpage;
				code = e.host;
			}
			// A compressed instruction in the last halfword of the page is
			// fetched as the two bytes it is; only a 4-byte one there needs
			// the next page, and so the ordinary step.
			uint32_t raw;
			if (off > 0xFFC) {
				uint16_t h; std::memcpy(&h, code + off, 2);
				raw = h;
				if ((h & 3) == 3) {
					// A 4-byte instruction across the page boundary: the two
					// halfword fetches fetch16 would make, both from cached pages.
					const FetchPage &n2 = fetch_cache[(vpage + 1) & (FETCH_CACHE_SIZE - 1)];
					if (!(n2.vpage == vpage + 1 && n2.key == key)) { fs_fetch++; break; }
					uint16_t h2; std::memcpy(&h2, n2.host, 2);
					raw |= (uint32_t)h2 << 16;
				}
			} else {
				std::memcpy(&raw, code + off, 4);
			}
			const bool comp = (raw & 3) != 3;
			const uint32_t tag = comp ? (raw & 0xFFFF) : raw;
			// The compact entry at the decode cache's index, found by byte
			// offset as decode_and_dispatch finds its own.
			const uint64_t doff = (pc << 3) & ((uint64_t)Decoder::CACHE_MASK << 4);
			const Decoder::FastEntry &d = *reinterpret_cast<const Decoder::FastEntry *>(
				reinterpret_cast<const char *>(ftab.data()) + doff);
			if (d.raw != tag) { fs_decode++; break; }
			const uint64_t len = d.length;
			const uint64_t a = regs.read_x(d.rs1), b = regs.read_x(d.rs2);
			const uint64_t imm = (uint64_t)(int64_t)d.imm;
			uint64_t next = pc + len;
			switch ((FastOp)d.fast_op) {
			case FOP_SLOW: fs_ext[d.ext & 63]++; fs_op[d.opcode]++; goto out;
			case FOP_LUI:   regs.write_x(d.rd, imm); break;
			case FOP_AUIPC: regs.write_x(d.rd, pc + imm); break;
			case FOP_JAL:   regs.write_x(d.rd, next); next = pc + imm; break;
			case FOP_JALR: { const uint64_t t = (a + imm) & ~1ull; regs.write_x(d.rd, next); next = t; break; }
			case FOP_BEQ:  if (a == b) next = pc + imm; break;
			case FOP_BNE:  if (a != b) next = pc + imm; break;
			case FOP_BLT:  if ((int64_t)a < (int64_t)b) next = pc + imm; break;
			case FOP_BGE:  if ((int64_t)a >= (int64_t)b) next = pc + imm; break;
			case FOP_BLTU: if (a < b) next = pc + imm; break;
			case FOP_BGEU: if (a >= b) next = pc + imm; break;
			case FOP_LB: case FOP_LH: case FOP_LW: case FOP_LD:
			case FOP_LBU: case FOP_LHU: case FOP_LWU: {
				static const uint8_t w[] = {0,0,0,0,0,0,0,0,0,0,0, 1,2,4,8,1,2,4};
				const unsigned size = w[d.fast_op];
				const uint64_t addr = a + imm;
				if (((addr + size - 1) >> 12) != (addr >> 12)) goto out;
				const RiscvCore::DataPage &e = core.load_cache[(addr >> 12) & (RiscvCore::DATA_CACHE_SIZE - 1)];
				if (!(e.vpage == (addr >> 12) && e.key == dkey)) { fs_ld++; goto out; }
				uint64_t v = 0;
				std::memcpy(&v, e.host + (addr & 0xFFF), size);
				switch (d.fast_op) {
				case FOP_LB: v = (uint64_t)(int64_t)(int8_t)v; break;
				case FOP_LH: v = (uint64_t)(int64_t)(int16_t)v; break;
				case FOP_LW: v = (uint64_t)(int64_t)(int32_t)v; break;
				default: break;
				}
				regs.write_x(d.rd, v);
				break;
			}
			case FOP_SB: case FOP_SH: case FOP_SW: case FOP_SD: {
				const unsigned size = 1u << (d.fast_op - FOP_SB);
				const uint64_t addr = a + imm;
				if (((addr + size - 1) >> 12) != (addr >> 12)) goto out;
				const RiscvCore::DataPage &e = core.store_cache[(addr >> 12) & (RiscvCore::DATA_CACHE_SIZE - 1)];
				if (!(e.vpage == (addr >> 12) && e.key == dkey)) { fs_st++; goto out; }
				std::memcpy(e.host + (addr & 0xFFF), &b, size);
				if (e.backing != Memory::Backing::Ram) memory.framebuffer_stored(e.backing, size);
				break;
			}
			case FOP_ADDI:  regs.write_x(d.rd, a + imm); break;
			case FOP_SLTI:  regs.write_x(d.rd, (int64_t)a < (int64_t)d.imm); break;
			case FOP_SLTIU: regs.write_x(d.rd, a < imm); break;
			case FOP_XORI:  regs.write_x(d.rd, a ^ imm); break;
			case FOP_ORI:   regs.write_x(d.rd, a | imm); break;
			case FOP_ANDI:  regs.write_x(d.rd, a & imm); break;
			case FOP_SLLI:  regs.write_x(d.rd, a << (d.imm & 0x3F)); break;
			case FOP_SRLI:  regs.write_x(d.rd, a >> (d.imm & 0x3F)); break;
			case FOP_SRAI:  regs.write_x(d.rd, (uint64_t)((int64_t)a >> (d.imm & 0x3F))); break;
			case FOP_ADDIW: regs.write_x(d.rd, sext32((uint32_t)a + (uint32_t)d.imm)); break;
			case FOP_SLLIW: regs.write_x(d.rd, sext32((uint32_t)a << (d.imm & 0x1F))); break;
			case FOP_SRLIW: regs.write_x(d.rd, sext32((uint32_t)a >> (d.imm & 0x1F))); break;
			case FOP_SRAIW: regs.write_x(d.rd, sext32((uint32_t)((int32_t)(uint32_t)a >> (d.imm & 0x1F)))); break;
			case FOP_ADD:  regs.write_x(d.rd, a + b); break;
			case FOP_SUB:  regs.write_x(d.rd, a - b); break;
			case FOP_SLL:  regs.write_x(d.rd, a << (b & 0x3F)); break;
			case FOP_SLT:  regs.write_x(d.rd, (int64_t)a < (int64_t)b); break;
			case FOP_SLTU: regs.write_x(d.rd, a < b); break;
			case FOP_XOR:  regs.write_x(d.rd, a ^ b); break;
			case FOP_SRL:  regs.write_x(d.rd, a >> (b & 0x3F)); break;
			case FOP_SRA:  regs.write_x(d.rd, (uint64_t)((int64_t)a >> (b & 0x3F))); break;
			case FOP_OR:   regs.write_x(d.rd, a | b); break;
			case FOP_AND:  regs.write_x(d.rd, a & b); break;
			case FOP_ADDW: regs.write_x(d.rd, sext32((uint32_t)a + (uint32_t)b)); break;
			case FOP_SUBW: regs.write_x(d.rd, sext32((uint32_t)a - (uint32_t)b)); break;
			case FOP_SLLW: regs.write_x(d.rd, sext32((uint32_t)a << (b & 0x1F))); break;
			case FOP_SRLW: regs.write_x(d.rd, sext32((uint32_t)a >> (b & 0x1F))); break;
			case FOP_SRAW: regs.write_x(d.rd, sext32((uint32_t)((int32_t)(uint32_t)a >> (b & 0x1F)))); break;
			case FOP_FENCE: break;
			case FOP_FLW: case FOP_FLD: {
				// As exec_F/exec_D do it: FLW NaN-boxes the word.
				if (!fp_ok) goto out;
				const unsigned size = d.fast_op == FOP_FLD ? 8 : 4;
				const uint64_t addr = a + imm;
				if (((addr + size - 1) >> 12) != (addr >> 12)) goto out;
				const RiscvCore::DataPage &e = core.load_cache[(addr >> 12) & (RiscvCore::DATA_CACHE_SIZE - 1)];
				if (!(e.vpage == (addr >> 12) && e.key == dkey)) { fs_ld++; goto out; }
				uint64_t v = 0;
				std::memcpy(&v, e.host + (addr & 0xFFF), size);
				regs.write_f(d.rd, f64_from_bits(size == 4 ? box_f32((uint32_t)v) : v));
				break;
			}
			case FOP_FSW: case FOP_FSD: {
				if (!fp_ok) goto out;
				const unsigned size = d.fast_op == FOP_FSD ? 8 : 4;
				const uint64_t addr = a + imm;
				if (((addr + size - 1) >> 12) != (addr >> 12)) goto out;
				const RiscvCore::DataPage &e = core.store_cache[(addr >> 12) & (RiscvCore::DATA_CACHE_SIZE - 1)];
				if (!(e.vpage == (addr >> 12) && e.key == dkey)) { fs_st++; goto out; }
				const uint64_t bits = bits_from_f64(regs.read_f(d.rs2));
				std::memcpy(e.host + (addr & 0xFFF), &bits, size);
				if (e.backing != Memory::Backing::Ram) memory.framebuffer_stored(e.backing, size);
				break;
			}
			case FOP_FEXT: {
				if (!fp_ok) goto out;
				// The full decode, at the same index -- as for FOP_MEXT.
				const DecodedOp &full = reinterpret_cast<const Decoder::CacheEntry *>(
					reinterpret_cast<const char *>(dcache.data()) + (doff << 1))->decoded;
				if (full.raw != tag) goto out;
				// A reserved rounding mode is an illegal instruction: step()'s.
				if (fp_rm_illegal(full, regs.get_frm())) goto out;
				regs.set_pc(pc);
				if (full.ext == Extension::D) core.exec_D(full, regs, memory);
				else core.exec_F(full, regs, memory);
				next = regs.get_pc();
				break;
			}
			case FOP_MEXT: {
				// exec_32M takes the full decode: the entry at the same index in
				// the full cache, written with this one.
				const DecodedOp &full = reinterpret_cast<const Decoder::CacheEntry *>(
					reinterpret_cast<const char *>(dcache.data()) + (doff << 1))->decoded;
				if (full.raw != tag) goto out;
				// exec_32M reads pc from regs and sets it itself.
				regs.set_pc(pc);
				core.exec_32M(full, regs, memory);
				next = regs.get_pc();
				break;
			}
			}
			hist[hp] = { pc, tag };
			hp = (hp + 1) & (Registers::HISTORY_SIZE - 1);
			pc = next;
			last_insn = tag;
			last_len = (uint8_t)len;
			done++;
		}
	out:
		regs.history_index() = (int)hp;
		regs.set_pc(pc);
		fs_fast += done;
		if (done == limit && limit < n) fs_deadline++;
		if (done) {
			// What the `done` steps' end_step and step_instructions would have
			// done, one step at a time.
			memory.step_instructions((uint32_t)done);
			if (regs.minstret_increment) regs.csr_add(0xB02, done);
			const uint64_t ticks = (tick_phase + done) / INSNS_PER_TICK;
			tick_phase = (uint32_t)((tick_phase + done) % INSNS_PER_TICK);
			if (ticks) {
				if (counts_cycle) regs.csr_add(0xB00, ticks);
				timer.tick((uint32_t)ticks);
			}
			step_committed = true;
			step_decoded = true;
			step_insn = last_insn;
			step_insn_len = last_len;
			n -= done;
		}
		// The step that ended the run, if it was not the budget.
		if (done < limit && n > 0 && !debugger.halted) {
			step();
			n--;
		}
	}
}

void DoomSystem::step()
{
	const uint64_t before = memory.instruction_count();
	step_execute();
	if (memory.instruction_count() != before) end_step();
}

// A tick of Sail's clock: mtime, and mcycle unless it is inhibited or
// filtered out in the current mode.
void DoomSystem::clock_tick()
{
	Registers &regs = cur->regs;
	if (regs.keyed(regs.state_gen + ExtensionsEpoch) != counter_key) refresh_counter_enables();
	if (counts_cycle) regs.bump_csr(0xB00);
	memory.tick_clock();
}

// After a step: minstret if the instruction completed and counts, and the
// clock every INSNS_PER_TICK steps.
void DoomSystem::end_step()
{
	Registers &regs = cur->regs;
	if (step_committed && regs.minstret_increment) regs.bump_csr(0xB02);
	if (multi) {
		// The round moves the clock (run_round).
		cur->steps++;
		cur->retired = true;
		return;
	}
	if (++tick_phase == INSNS_PER_TICK) {
		tick_phase = 0;
		clock_tick();
	}
}

// A WFI or WRS waits as Sail's simulator waits. Entering the wait takes no
// step but ticks the clock; each further round checks whether the wait is
// over and otherwise ticks, for at most MAX_WAIT_TICKS ticks. It is over when
// an interrupt is pending and enabled, when a WRS has no reservation, or when
// it times out, where a WFI below M or a wrs.nto may trap instead of
// completing. Only then is it a step, the WFI's own, with pc moving past it.
// The ticks run whatever the host does, so a wait is as deterministic as any
// other instruction.
void DoomSystem::run_wait()
{
	Registers &regs = cur->regs;
	RiscvCore &core = cur->core;
	const RiscvCore::Wait kind = core.wait_request;
	core.wait_request = RiscvCore::Wait::None;

	constexpr uint64_t TW = 1ull << 21, VTW = 1ull << 21;
	uint32_t remaining = MAX_WAIT_TICKS;
	clock_tick();
	int trap = 0;   // 2 illegal, 22 virtual instruction
	for (;;) {
		const bool timed_out = remaining == 0;
		if (regs.keyed(regs.state_gen + ExtensionsEpoch) != counter_key) refresh_counter_enables();
		regs.minstret_increment = counts_instret;
		if (core.wake_for_interrupt(regs, memory)) break;
		if (kind != RiscvCore::Wait::Wfi && !core.reservation_held()) break;
		if (timed_out) {
			const PrivMode p = regs.get_priv();
			const bool v = Extensions.H && regs.get_virt();
			const bool tw = (regs.read_csr(0x300) & TW) != 0;
			const bool vtw = v && (regs.read_csr(0x600) & VTW) != 0;
			if (kind == RiscvCore::Wait::Wfi) {
				if (p == PrivMode::S && v) trap = vtw ? 22 : 0;
				else if (p != PrivMode::M) trap = tw ? 2 : 0;
			} else if (kind == RiscvCore::Wait::WrsNto && p != PrivMode::M) {
				trap = tw ? 2 : (vtw ? 22 : 0);
			}
			break;
		}
		if (--remaining > 0) clock_tick();
	}

	if (trap == 2) core.raise_illegal_instruction(regs, step_insn);
	else if (trap == 22) core.raise_virtual_instruction(regs, step_insn);
	else regs.set_pc(regs.get_pc() + step_insn_len);
	if (trap) step_committed = false;
}

// One halfword of an instruction fetch: translate_or_trap and read16, as
// the fetch has always been, or the same answer from a cached page.
//
// A page is cached once a fetch from it has succeeded through the full path
// and the page is one the architecture would answer identically for every
// fetch inside it: no second translation stage, RAM from end to end, and --
// with PMP -- one entry deciding the whole page (pmp::page_permits). The
// entry holds the translation and the permission, never the bytes: those are
// read from RAM on every fetch, so code that is written to is fetched as
// written. It is dropped by anything that could change the answer: a CSR
// write, a change of privilege or V (both regs.state_gen, which covers satp,
// the PMP CSRs and mstatus), a TLB flush (sfence.vma, and everything that
// flushes the TLB for its own reasons), or a change to the extensions.
bool DoomSystem::fetch16(uint64_t vaddr, uint16_t &out)
{
	Registers &regs = cur->regs;
	RiscvCore &core = cur->core;
	FetchPage *const fetch_cache = cur->fetch_cache;
	const uint64_t key = regs.fetch_key(regs.state_gen + mmu_tlb_generation() + ExtensionsEpoch);
	const uint64_t vpage = vaddr >> 12;
	const unsigned offset = (unsigned)(vaddr & 0xFFF);
	FetchPage &e = fetch_cache[vpage & (FETCH_CACHE_SIZE - 1)];
	if (e.vpage == vpage && e.key == key && offset <= 0xFFE) {
		std::memcpy(&out, e.host + offset, sizeof(out));
		return true;
	}

	uint64_t paddr;
	if (!core.translate_or_trap(regs, memory, vaddr, AccessType::Fetch, paddr, 2)) return false;
	out = memory.read16(paddr);

	const uint64_t ppage = paddr & ~0xFFFull;
	if (offset <= 0xFFE && !(Extensions.H && regs.get_virt()) && Memory::in_ram(ppage, 0x1000)
	    && (!Extensions.SMPMP
	        || pmp::page_permits(regs, ppage, pmp::ACC_FETCH, (uint8_t)regs.get_priv()))) {
		e.vpage = vpage;
		e.key = key;
		e.host = memory.ram_data() + (ppage - Memory::RAM_BASE);
	}
	return true;
}

// A whole instruction in one cache lookup, where that is exactly what the two
// halfword fetches below would have done anyway.
//
// fetch16 splits a fetch in two because the architecture checks a halfword at
// a time: the two halves of a 4-byte instruction can sit in different pages,
// or on opposite sides of a PMP region's edge, and answer differently. None of
// that is possible when both halves are in the *same* page and that page is
// already in the fetch cache -- getting in there required the page to be plain
// RAM, not a guest's second-stage mapping, and fetchable under PMP as a whole
// (see the tail of fetch16). Within such a page the second halfword cannot
// fault, cannot hit an MMIO side effect, and cannot answer differently from
// the first. So one read of four bytes is the same answer as two reads of two,
// for a third of the per-instruction fetch work.
//
// The offset bound is 0xFFC rather than fetch16's 0xFFE because this reads
// four bytes, not two. Anything past it -- and every miss -- falls back to the
// halfword path, which stays the definition of what a fetch means.
//
// A compressed instruction only occupies the low half, and this reads the two
// bytes after it as well. They are ordinary RAM in a page already proven
// fetchable, so reading them has no effect the guest can observe, and
// step_execute masks them off before anything records the encoding.
bool DoomSystem::fetch_instr(uint64_t vaddr, uint32_t &out)
{
	Registers &regs = cur->regs;
	FetchPage *const fetch_cache = cur->fetch_cache;
	const uint64_t key = regs.fetch_key(regs.state_gen + mmu_tlb_generation() + ExtensionsEpoch);
	const uint64_t vpage = vaddr >> 12;
	const unsigned offset = (unsigned)(vaddr & 0xFFF);
	const FetchPage &e = fetch_cache[vpage & (FETCH_CACHE_SIZE - 1)];
	if (e.vpage == vpage && e.key == key && offset <= 0xFFC) {
		uint32_t word;
		std::memcpy(&word, e.host + offset, sizeof(word));
		out = word;
		return true;
	}

	uint16_t half;
	if (!fetch16(vaddr, half)) return false;
	out = half;
	if ((out & 0x3) == 0x3) {
		if (!fetch16(vaddr + 2, half)) return false;
		out |= (uint32_t)half << 16;
	}
	return true;
}

void DoomSystem::step_execute()
{
	Registers &regs = cur->regs;
	RiscvCore &core = cur->core;
	Decoder &decoder = cur->decoder;
	if (debugger.halted) return;
	const uint64_t traps_before = core.trap_count;
	step_committed = false;
	step_decoded = false;
	if (regs.keyed(regs.state_gen + ExtensionsEpoch) != counter_key) refresh_counter_enables();
	regs.minstret_increment = counts_instret;

	// A hart in a WFI or WRS, with others running: a round of the wait,
	// which takes no interrupt -- one that wakes it is taken in the step
	// after the instruction completes, as Sail's try_step does.
	if (multi && cur->waiting) {
		wait_slot();
		return;
	}

	// In lenient lock-step the reference decides when an interrupt is taken
	// (see traced_step), so the machine's own devices never interrupt by
	// themselves. Strict lock-step takes them as ever, and checks the timing.
	// The same test interrupt_may_be_due opens with, inline: on most steps
	// it is all there is.
	const bool irq_unchanged = EventGen == irq_key
	                        && memory.get_timer().get_mtime() < irq_deadline;
	if (!(lockstep_active && !lockstep_strict) && !irq_unchanged && interrupt_may_be_due()
	    && core.check_and_take_interrupt(regs, memory)) {
		// pc has already been redirected into the trap handler -- this
		// "step" was the interrupt itself, not whatever instruction was
		// about to execute at the old pc.
		memory.step_instructions(1);
		return;
	}

	uint64_t pc = regs.get_pc();
	if (debugger.may_halt() && debugger.should_halt(pc, false)) {
		console_drain();
		debugger.dump_log(regs, memory, "crash.log");
		if (has_sig_range) debugger.dump_signature(memory, sig_begin, sig_end, sig_path.c_str());
		dump_framebuffer();
		run_finished = true;
		return;
	}

	// Fetch a halfword at a time, because that is the unit the
	// architecture checks. A four-byte instruction may sit across a page
	// boundary or across the edge of a PMP region, with the two halves
	// answering differently -- and with C, an instruction can start on any
	// even address, so this is ordinary rather than exotic. Translating
	// once at pc and reading four bytes gets the second half from whatever
	// happened to follow the first page, silently.
	//
	// The length is in the low two bits of the first halfword, so the
	// second fetch only happens when there really is a second halfword.
	// With C off, only a 4-byte-aligned pc is an instruction start.
	if (!Extensions.C && (pc & 2)) {
		core.raise_misaligned_fetch(regs, pc);
		memory.step_instructions(1);
		return;
	}

	uint32_t instr;
	if (!fetch_instr(pc, instr)) {
		// A page fault redirected pc into the trap handler already --
		// nothing more to do for this step.
		memory.step_instructions(1);
		return;
	}
	step_decoded = true;
	DispatchResult result = decoder.decode_and_dispatch(pc, instr);
	// A compressed instruction's raw fetch also contains the next
	// instruction's bytes in its upper half -- mask those off so the
	// trace log/crash dump show just the actual 16-bit encoding.
	uint32_t recorded_instr = (result.decoded->length == 2) ? (instr & 0xFFFF) : instr;
	step_insn = recorded_instr;
	step_insn_len = result.decoded->length;
	// Committed: it ran to completion -- not illegal, and no trap taken while
	// it executed, such as a page fault or an ecall.
	step_committed = !result.illegal && core.trap_count == traps_before;
	if (core.wait_request != RiscvCore::Wait::None) {
		if (multi) {
			begin_wait(recorded_instr, (uint8_t)step_insn_len);
			return;
		}
		run_wait();
	}
	regs.record_history(pc, recorded_instr);

	// A test that signals completion through HTIF stops here, with its
	// signature dumped exactly as a breakpoint would. This is what lets a
	// suite that exports only `tohost` -- no `pass` label to break on -- be
	// run at all.
	if (memory.tohost_written()) {
		debugger.halted = true;
		console_drain();
		debugger.dump_log(regs, memory, "crash.log");
		if (has_sig_range) debugger.dump_signature(memory, sig_begin, sig_end, sig_path.c_str());
		// The verdict goes in its own file rather than being read back out
		// of the tohost word: acknowledging a console write zeroes that
		// word, so by the time anything looks at memory the value is gone.
		// A harness watching for this file also gets a stop signal that
		// does not depend on a signature range existing.
		{
			std::ofstream f("tohost.log");
			if (f) f << std::hex << memory.tohost_written() << std::endl;
		}
		// Last, after tohost.log: a headless run exits the instant this is
		// set, and the harness reads that file for the verdict.
		run_finished = true;
		memory.step_instructions(1);
		return;
	}

	if (debugger.may_halt() && debugger.should_halt(pc, result.illegal)) {
		if (result.illegal) {
			// Remember what to raise on resume. recorded_instr is the
			// encoding as actually fetched (masked to 16 bits for a
			// compressed one), which is what the spec wants in [ms]tval.
			pending_illegal = true;
			pending_illegal_tval = recorded_instr;
		}
		console_drain();
		debugger.dump_log(regs, memory, "crash.log");
		if (has_sig_range) debugger.dump_signature(memory, sig_begin, sig_end, sig_path.c_str());
		run_finished = true;
	} else if (result.illegal) {
		// Not halting, so the guest gets its trap. This is the ordinary
		// path now: a guest with a handler is entitled to take the
		// exception and carry on, and the conformance suite depends on it
		// -- several tests execute an illegal instruction on purpose.
		//
		// tval is the encoding as fetched, masked to 16 bits for a
		// compressed one, which is what the spec asks for.
		core.raise_illegal_instruction(regs, recorded_instr);
		memory.step_instructions(1);
		return;
	}

	memory.step_instructions(1);
}

void DoomSystem::watch_tohost(uint64_t addr)
{
	memory.watch_tohost(addr);
}


// Writes the Linux framebuffer as a binary PPM -- the simplest format that
// needs no library and that anything can read. Called wherever a run stops.
void DoomSystem::dump_framebuffer()
{
	if (fb_dump_path.empty()) return;
	// After the guest output that came before it, not interleaved with it.
	console_drain();
	write_framebuffer_dump(reinterpret_cast<const uint32_t *>(memory.linux_framebuffer()));
}

// The periodic refresh used to be every 200th snapshot publish, on the CPU
// thread: a pass over 1.2M pixels and a 3.7MB write, a few seconds apart,
// for a file only the host reads. It is a copy and a write here instead,
// while the guest keeps running. A copy taken while the guest is drawing can
// be half old and half new, which for a debugging picture refreshed every
// few seconds does not matter; the dump written when a run stops is exact.
void DoomSystem::fbdump_loop()
{
	using clock = std::chrono::steady_clock;
	std::vector<uint32_t> copy((size_t)Memory::LFB_W * Memory::LFB_H);
	clock::time_point next = clock::now() + std::chrono::seconds(5);
	while (!stopping) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (clock::now() < next) continue;
		next = clock::now() + std::chrono::seconds(5);
		std::memcpy(copy.data(), memory.linux_framebuffer(), copy.size() * sizeof(uint32_t));
		write_framebuffer_dump(copy.data());
	}
}

void DoomSystem::write_framebuffer_dump(const uint32_t *px)
{
	std::lock_guard<std::mutex> lock(fb_dump_mutex);
	std::ofstream f(fb_dump_path, std::ios::binary);
	if (!f.is_open()) return;
	f << "P6\n" << Memory::LFB_W << " " << Memory::LFB_H << " " << 255 << "\n";
	uint64_t nonzero = 0;
	for (size_t i = 0; i < (size_t)Memory::LFB_W * Memory::LFB_H; i++) {
		const uint32_t v = px[i];
		if (v & 0x00FFFFFFu) nonzero++;
		const char rgb[3] = { (char)((v >> 16) & 0xFF), (char)((v >> 8) & 0xFF), (char)(v & 0xFF) };
		f.write(rgb, 3);
	}
	// The count is the answer to the question this was added for: whether
	// anything has been drawn at all. A picture needs a viewer; a number
	// does not.
	std::cout << "framebuffer: " << nonzero << " of "
	          << (uint64_t)Memory::LFB_W * Memory::LFB_H
	          << " pixels non-black, written to " << fb_dump_path << "\n";
	std::cout.flush();
}

void DoomSystem::publish_snapshot()
{
	// Hart 0's, whichever hart stepped last: the dashboard shows one hart,
	// and a steady one. Its CSRs are read through Memory, which answers for
	// the hart selected there, so that is hart 0 for the duration.
	Hart &shown = *harts[0];
	Registers &regs = shown.regs;
	RiscvCore &core = shown.core;
	Decoder &decoder = shown.decoder;
	const unsigned was = memory.current_hart();
	memory.select_hart(0);
	struct Reselect {
		Memory &m;
		unsigned h;
		~Reselect() { m.select_hart(h); }
	} reselect{memory, was};
	Snapshot snap;
	snap.seq = ++snapshot_seq;

	for (int i = 0; i < 32; i++) snap.x[i] = regs.read_x(i);
	for (int i = 0; i < 32; i++) std::memcpy(&snap.v_lo[i], regs.read_v(i), sizeof(uint64_t));
	snap.pc = regs.get_pc();
	snap.halted = debugger.halted;

	const auto entry = [&](int index) {
		const Registers::HistoryRecord &h = regs.history_at(index);
		return HistoryEntry{ h.pc, h.instr, decoder.describe(h.instr) };
	};
	int active_idx = (regs.history_pos() + Registers::HISTORY_SIZE - 1) % Registers::HISTORY_SIZE;
	snap.active = entry(active_idx);
	for (int i = 0; i < 13; i++) {
		int pos = (regs.history_pos() + i) % Registers::HISTORY_SIZE;
		snap.trace[i] = entry(pos);
	}

	uint16_t top[Snapshot::CSR_PANEL_SIZE];
	snap.csr_count = regs.top_csrs(top, Snapshot::CSR_PANEL_SIZE);
	for (int i = 0; i < snap.csr_count; i++)
		snap.csrs[i] = { top[i], core.read_csr_effective(regs, memory, top[i]) };

	std::lock_guard<std::mutex> lock(snapshot_mutex);
	shared_snapshot = std::move(snap);
}

// Whole frames out of the guest framebuffer, on a thread of its own.
//
// The framebuffer used to be copied once per CPU burst, wherever the guest
// had got to. A guest draws a frame over many bursts -- DOOM's 64000 pixels
// take a tenth of a second of emulated work, and fbcon scrolling a full
// console takes longer -- so what reached the window was the frame being
// built, a band at a time: new text appearing line by line, a scroll wiping
// down the screen, DOOM's view tearing.
//
// Neither framebuffer has a way to say "this frame is done": simple-
// framebuffer has no page flip, and DOOM's MMIO buffer is written in place.
// What they do have is a shape. A frame is drawn in one burst of writes, and
// then the guest goes and does something else -- runs the game logic, waits
// for the next printk, handles an event. So a frame is taken once the writes
// have stopped for QUIET, and the copy is checked against the generation
// counter afterwards: if a write landed while copying, the guest has started
// the next frame and this one is thrown away rather than shown torn.
//
// MAX_HOLD bounds how long a picture can stay stale. A guest that draws
// without ever pausing -- a kernel log scrolling for seconds -- would
// otherwise freeze the display until it stopped, so past MAX_HOLD the frame
// is taken as it stands, once, and the wait starts again.
//
// Reading the framebuffer while the CPU thread writes it is a race on plain
// bytes, which on this host is the price of one torn pixel at worst -- and
// the generation check means a torn copy is never the one shown unless
// MAX_HOLD forced it.
void DoomSystem::display_loop()
{
	using clock = std::chrono::steady_clock;
	const auto QUIET = std::chrono::milliseconds(12);
	const auto MAX_HOLD = std::chrono::milliseconds(500);

	const bool lfb = linux_mode;
	const size_t pixels = lfb ? (size_t)Memory::LFB_W * Memory::LFB_H
	                          : (size_t)Memory::FB_W * Memory::FB_H;
	const uint8_t *src = lfb ? memory.linux_framebuffer() : memory.framebuffer();
	const auto generation = [&] { return lfb ? memory.lfb_generation() : memory.fb_generation(); };

	std::vector<uint32_t> buf(pixels);
	uint64_t seen = generation();
	uint64_t shown = seen - 1;   // differs from everything: the first frame is always taken
	clock::time_point last_change = clock::now();
	clock::time_point dirty_since = last_change;

	while (!stopping) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		const clock::time_point now = clock::now();

		const uint64_t g = generation();
		if (g != seen) {
			if (seen == shown) dirty_since = now;
			seen = g;
			last_change = now;
		}
		if (seen == shown) continue;

		const bool quiet = now - last_change >= QUIET;
		const bool stale = now - dirty_since >= MAX_HOLD;
		if (!quiet && !stale) continue;

		const uint64_t before = generation();
		std::memcpy(buf.data(), src, pixels * sizeof(uint32_t));
		const uint64_t after = generation();
		if (after != before && !stale) {
			// The guest started drawing again mid-copy.
			seen = after;
			last_change = clock::now();
			continue;
		}

		gui.submit_frame(buf);
		shown = before;
		if (after != before) {
			seen = after;
			dirty_since = clock::now();
		}
	}
}

// The CSRs, register file and trace log, on a thread of their own. Formatting
// and drawing some sixty lines of text is not free, and it used to happen on
// the window thread for every frame whether or not the machine had moved.
// This draws each published snapshot once, into the dashboard's own layer,
// and the window only composites the result.
void DoomSystem::dashboard_loop()
{
	uint64_t drawn = 0;
	while (!stopping) {
		Snapshot snap;
		bool fresh = false;
		{
			std::lock_guard<std::mutex> lock(snapshot_mutex);
			if (shared_snapshot.seq != drawn) {
				snap = shared_snapshot;
				fresh = true;
			}
		}
		if (fresh) {
			gui.render_dashboard(snap);
			drawn = snap.seq;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
}

void DoomSystem::move_pointer(int x, int y)
{
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= Memory::LFB_W) x = Memory::LFB_W - 1;
	if (y >= Memory::LFB_H) y = Memory::LFB_H - 1;
	GuestInput in;
	in.kind = GuestInput::Mouse;
	in.a = VirtioInput::EV_ABS;
	in.b = VirtioInput::ABS_X; in.c = x; submit_input(in);
	in.b = VirtioInput::ABS_Y; in.c = y; submit_input(in);
	// One SYN for the pair: a report is a complete state change, and
	// splitting X from Y makes a diagonal movement arrive as two steps.
	in.a = VirtioInput::EV_SYN; in.b = VirtioInput::SYN_REPORT; in.c = 0; submit_input(in);
}

void DoomSystem::commit_pointer(uint64_t now, int x, int y)
{
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= Memory::LFB_W) x = Memory::LFB_W - 1;
	if (y >= Memory::LFB_H) y = Memory::LFB_H - 1;
	GuestInput in;
	in.kind = GuestInput::Mouse;
	in.a = VirtioInput::EV_ABS;
	in.b = VirtioInput::ABS_X; in.c = x; commit_input(now, in);
	in.b = VirtioInput::ABS_Y; in.c = y; commit_input(now, in);
	in.a = VirtioInput::EV_SYN; in.b = VirtioInput::SYN_REPORT; in.c = 0; commit_input(now, in);
}

void DoomSystem::stop_at_limit()
{
	Registers &regs = cur->regs;
	// Exact, not approximate: every step, trap or instruction, advances the
	// step count by one, so the first check to see stop_at is the one
	// straight after that instruction.
	const uint64_t n = memory.instruction_count();
	stop_at = 0;
	debugger.halted = true;
	console_drain();
		debugger.dump_log(regs, memory, "crash.log");
	// With several harts the log above is the hart that stepped last; every
	// hart's place follows it.
	if (multi) {
		std::ofstream f("crash.log", std::ios::app);
		f << std::hex << std::setfill('0');
		for (const auto &h : harts)
			f << "hart " << std::dec << h->id << std::hex << " pc " << std::setw(16) << h->regs.get_pc()
			  << " priv " << (int)h->regs.get_priv() << " steps " << std::dec << h->steps
			  << (h->waiting ? " waiting" : "") << std::hex << '\n';
	}
	std::cout << "stopped after instruction " << n << "; state in crash.log" << std::endl;
	if (std::getenv("DOOMV_FASTSTATS")) {
		std::printf("fast steps %llu (%.1f%%)\nslow at run entry (irq/state checks) %llu\nruns ended by irq deadline %llu\n"
		            "fetch miss %llu\ndecode miss %llu\nload miss %llu\nstore miss %llu\n",
			(unsigned long long)fs_fast, 100.0 * fs_fast / n, (unsigned long long)fs_slow_entry, (unsigned long long)fs_deadline,
			(unsigned long long)fs_fetch, (unsigned long long)fs_decode, (unsigned long long)fs_ld, (unsigned long long)fs_st);
		static const char *const entry_why[6] = {"debug, logs or config", "extension epoch", "a wait", "counter key", "event key", "interrupt deadline"};
		for (int i = 0; i < 6; i++) if (fs_entry[i]) std::printf("run entry refused, %s: %llu\n", entry_why[i], (unsigned long long)fs_entry[i]);
		for (int i = 0; i < 64; i++) if (fs_ext[i]) std::printf("slow op ext %d: %llu\n", i, (unsigned long long)fs_ext[i]);
		for (int i = 0; i < 128; i++) if (fs_op[i]) std::printf("slow op opcode 0x%02x: %llu\n", i, (unsigned long long)fs_op[i]);
	}
	// Prototype check: what crash.log leaves out, for comparing the two loops.
	if (std::getenv("DOOMV_STATEDUMP")) {
		uint64_t h = 1469598103934665603ull;
		const uint8_t *ram = memory.ram_data();
		for (uint64_t i = 0; i + 8 <= Memory::RAM_SPAN; i += 8) { uint64_t w; std::memcpy(&w, ram + i, 8); h = (h ^ w) * 1099511628211ull; }
		uint64_t fh = 1469598103934665603ull;
		const uint8_t *fbp = memory.framebuffer();
		for (uint32_t i = 0; i < Memory::FB_SIZE; i++) fh = (fh ^ fbp[i]) * 1099511628211ull;
		const uint8_t *lfbp = memory.linux_framebuffer();
		for (uint64_t i = 0; i < Memory::LFB_SIZE; i++) fh = (fh ^ lfbp[i]) * 1099511628211ull;
		std::ofstream f("statedump.log");
		f << std::hex << "fb " << fh << " fb_gen " << memory.fb_generation() << " lfb_gen " << memory.lfb_generation()
		  << " fb_writes " << memory.take_fb_write_count() << "\nminstret " << regs.read_csr(0xB02) << "\nmcycle " << regs.read_csr(0xB00)
		  << "\nmtime " << memory.get_timer().get_mtime() << "\ntick_phase " << tick_phase
		  << "\nmmio_tick " << memory.read32(Memory::MMIO_TICK) << "\nsteps " << n
		  << "\npc " << regs.get_pc() << "\nram " << h << "\n";
	}

	run_finished = true;
}

void DoomSystem::resume_from_halt()
{
	Registers &regs = cur->regs;
	RiscvCore &core = cur->core;
	uint64_t pc = regs.get_pc();
	if (pending_illegal) {
		pending_illegal = false;
		core.raise_illegal_instruction(regs, pending_illegal_tval);
		memory.step_instructions(1);
		step_committed = false;
		end_step();
		// pc is now the trap handler, so there is no breakpoint to skip.
		debugger.halted = false;
		return;
	}
	debugger.resume(pc);
}

void DoomSystem::cpu_loop()
{
	// DOOMV_PROGRESS=<steps>: the step count and the time so far, each time the
	// run passes a multiple of it -- where a long boot spends its time. Host
	// output only, printed between bursts, so the guest cannot see it.
	const char *progress_env = std::getenv("DOOMV_PROGRESS");
	const uint64_t progress_every = progress_env ? std::strtoull(progress_env, nullptr, 0) : 0;
	uint64_t progress_next = progress_every ? (memory.instruction_count() / progress_every + 1) * progress_every : 0;
	const auto progress_start = std::chrono::steady_clock::now();
	while (true) {
		if (progress_every && memory.instruction_count() >= progress_next) {
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - progress_start).count();
			std::printf("progress: step %llu at %.1f s\n", (unsigned long long)memory.instruction_count(), s);
			std::fflush(stdout);
			progress_next = (memory.instruction_count() / progress_every + 1) * progress_every;
		}
		if (debugger.halted) {
			if (resume_requested.exchange(false)) {
				resume_from_halt();
			} else {
				// Keep publishing so the dashboard stays live while paused,
				// but do not spin a 200k-iteration burst doing nothing.
				publish_snapshot();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
		}

		// Bigger burst = more actual emulated work per snapshot-publish
		// overhead, since that overhead doesn't scale with burst size --
		// this raises total instructions/sec even though it lowers how
		// often the dashboard updates.
		// Input is committed at instruction counts, never at a point in a
		// burst: the burst boundaries depend on halts and resumes, and an
		// input delivered at "whenever the host got to it" is the one thing
		// that made two runs of the same guest differ. Every 4096
		// instructions is about 2.5kHz at the interpreter's speed, so a mouse
		// still feels attached.
		//
		// Every step that runs advances the count by exactly one, so the
		// steps up to the next input checkpoint or the -stopat limit can run
		// as a plain loop, with the checks made once at its end -- at the
		// same counts as checking after every step, which is what they used
		// to do. Lock-step keeps the step-by-step loop: a traced step can
		// decline to run while the reference record waits.
		int budget = 200000;
		while (budget > 0 && !debugger.halted) {
			const uint64_t before = memory.instruction_count();
			if (harts.size() > 1) {
				run_round();
				budget -= (int)harts.size();
			} else if (tracing) {
				traced_step();
				budget--;
			} else {
				uint64_t n = INPUT_PERIOD - (before & (INPUT_PERIOD - 1));
				if (stop_at) n = std::min<uint64_t>(n, stop_at > before ? stop_at - before : 1);
				if (snapshot_at > before) n = std::min<uint64_t>(n, snapshot_at - before);
				n = std::min<uint64_t>(n, (uint64_t)budget);
				if (fast_enabled()) run_fast(n);
				else for (uint64_t k = 0; k < n && !debugger.halted; k++) step();
				budget -= (int)n;
			}
			const uint64_t now = memory.instruction_count();
			if (now == before) {
				if (tracing) continue;
				break;
			}
			// A round of several harts can step over a multiple rather than
			// land on it; the checks are made at the round's end then.
			const bool rounds = harts.size() > 1;
			if ((now & (INPUT_PERIOD - 1)) == 0
			    || (rounds && now / INPUT_PERIOD != before / INPUT_PERIOD))
				service_input(now);
			// After the step's input, as a restored run resumes with the step
			// after it.
			if (snapshot_at && (now == snapshot_at || (rounds && before < snapshot_at && now > snapshot_at))) {
				snapshot_at = 0;
				save_snapshot(snapshot_dir);
			}
			if (stop_at && now >= stop_at) {
				stop_at_limit();
				break;
			}
		}
		publish_snapshot();

		// The guest asking to be turned off. Memory has recorded writes to
		// the sifive,test0 register since that device was added -- for
		// OpenSBI's benefit, which needs the compatible string to implement
		// SBI SRST at all -- but nothing ever read the flag back, so the
		// write was acknowledged and ignored. The visible symptom is the
		// kernel printing "reboot: Power down" and then, a second later,
		// "Unable to poweroff system", with the emulator carrying on: a
		// guest that cannot end its own run, which is the one thing the
		// device exists to make possible.
		//
		// Checked once per burst rather than per instruction. The guest is
		// in its poweroff path by then and has nothing left to do, so the
		// overrun costs nothing and a per-step check would sit on the hot
		// path.
		if (memory.poweroff_requested()) {
			dump_framebuffer();
			console_drain();
			std::cout << "guest requested poweroff" << std::endl;
			run_finished = true;
			return;
		}
	}
}

// Host stdin -> the guest's UART receive ring, for headless runs.
//
// The window's keyboard path (in run()) is the only way anything ever
// reached the guest console, which meant a -ng boot was
// write-only: you could read the kernel log and never answer it. That is
// fine for the test suites, which do not interact, and wrong for everything
// else -- logging in, running a command and reading what it printed, or
// telling a guest to power itself off, all of which had to be done by hand
// in a window. With this, a guest is scriptable from a pipe:
//
//   printf 'root\ndoomv\nuname -a\n' | riscv_doom.exe -ng -kernel=...
//
// Two details that are not optional:
//
// Newline translation. A pipe carries LF; a terminal sends CR when you press
// return, and the kernel's line discipline is configured for a terminal --
// ICRNL turns CR into LF on input and nothing turns LF into anything. Feed a
// raw LF and the shell sees a character it does not treat as end-of-line, so
// the command is typed and never run.
//
// Back-pressure. The ring is 16 bytes and the guest drains it at emulated
// speed, which is thousands of times slower than a pipe fills it. Dropping on
// full is right for a keyboard and useless here -- it would silently truncate
// every line past the first. So this blocks until each byte fits, which also
// paces the feed to whatever rate the guest is actually consuming at.
void DoomSystem::console_stdin_loop()
{
	// A live source -- a pipe or a terminal -- whose bytes arrive when the
	// host sends them. They are queued, and the CPU thread moves them into
	// the UART when it has room (see service_input), so what the guest sees
	// depends on when each one arrived: record the run with -record to
	// reproduce it. Stdin redirected from a file is read up front instead,
	// by prepare_input, and needs no recording.
	//
	// Deliberately does not end the run on EOF. A script that pipes in a few
	// commands and closes stdin still wants the guest to keep going and keep
	// printing; the run ends when the guest ends it, or when whoever started
	// it does.
	int c;
	while (!run_finished && (c = std::fgetc(stdin)) != EOF) {
		GuestInput in;
		in.kind = GuestInput::Uart;
		in.a = (uint8_t)(c == '\n' ? '\r' : c);
		submit_input(in);
	}
}


// SDL scancode -> Linux evdev keycode.
//
// A scancode, not a keysym, because that is the layer a keyboard reports
// at: evdev codes name *physical keys*, and the guest applies its own
// keymap to them. Translating from keysyms instead would apply the host's
// layout and then let the guest apply another one on top, so a Dvorak host
// driving a US guest would type mojibake. This way the guest's own
// `loadkeys` setting is what decides, exactly as on real hardware.
//
// The codes are from include/uapi/linux/input-event-codes.h. They are the
// classic AT set and all fall in 1..127, which is the range the keyboard
// device declares in its EV_BITS config (see VirtioInput::config_payload).
static uint16_t evdev_keycode(uint32_t scancode)
{
	switch (scancode) {
	// Letters. SDL orders these alphabetically and Linux orders them by
	// position on the board, so there is no arithmetic to be had here.
	case SDL_SCANCODE_A: return 30;
	case SDL_SCANCODE_B: return 48;
	case SDL_SCANCODE_C: return 46;
	case SDL_SCANCODE_D: return 32;
	case SDL_SCANCODE_E: return 18;
	case SDL_SCANCODE_F: return 33;
	case SDL_SCANCODE_G: return 34;
	case SDL_SCANCODE_H: return 35;
	case SDL_SCANCODE_I: return 23;
	case SDL_SCANCODE_J: return 36;
	case SDL_SCANCODE_K: return 37;
	case SDL_SCANCODE_L: return 38;
	case SDL_SCANCODE_M: return 50;
	case SDL_SCANCODE_N: return 49;
	case SDL_SCANCODE_O: return 24;
	case SDL_SCANCODE_P: return 25;
	case SDL_SCANCODE_Q: return 16;
	case SDL_SCANCODE_R: return 19;
	case SDL_SCANCODE_S: return 31;
	case SDL_SCANCODE_T: return 20;
	case SDL_SCANCODE_U: return 22;
	case SDL_SCANCODE_V: return 47;
	case SDL_SCANCODE_W: return 17;
	case SDL_SCANCODE_X: return 45;
	case SDL_SCANCODE_Y: return 21;
	case SDL_SCANCODE_Z: return 44;
	// Digit row. Both are contiguous, but SDL puts 0 after 9 and Linux
	// puts it after 9 as well -- KEY_1..KEY_0 is 2..11 -- so this one does
	// map arithmetically, and is written out anyway to stay checkable.
	case SDL_SCANCODE_1: return 2;
	case SDL_SCANCODE_2: return 3;
	case SDL_SCANCODE_3: return 4;
	case SDL_SCANCODE_4: return 5;
	case SDL_SCANCODE_5: return 6;
	case SDL_SCANCODE_6: return 7;
	case SDL_SCANCODE_7: return 8;
	case SDL_SCANCODE_8: return 9;
	case SDL_SCANCODE_9: return 10;
	case SDL_SCANCODE_0: return 11;
	case SDL_SCANCODE_MINUS:        return 12;
	case SDL_SCANCODE_EQUALS:       return 13;
	case SDL_SCANCODE_BACKSPACE:    return 14;
	case SDL_SCANCODE_TAB:          return 15;
	case SDL_SCANCODE_LEFTBRACKET:  return 26;
	case SDL_SCANCODE_RIGHTBRACKET: return 27;
	case SDL_SCANCODE_RETURN:       return 28;
	case SDL_SCANCODE_SEMICOLON:    return 39;
	case SDL_SCANCODE_APOSTROPHE:   return 40;
	case SDL_SCANCODE_GRAVE:        return 41;
	case SDL_SCANCODE_BACKSLASH:    return 43;
	case SDL_SCANCODE_NONUSHASH:    return 43; // same key on ISO boards
	case SDL_SCANCODE_NONUSBACKSLASH: return 86;
	case SDL_SCANCODE_COMMA:        return 51;
	case SDL_SCANCODE_PERIOD:       return 52;
	case SDL_SCANCODE_SLASH:        return 53;
	case SDL_SCANCODE_SPACE:        return 57;
	case SDL_SCANCODE_ESCAPE:       return 1;
	case SDL_SCANCODE_CAPSLOCK:     return 58;
	// Modifiers. These have to be forwarded as keys of their own: the
	// guest tracks shift/ctrl/alt state itself from press and release, and
	// a host-side "this keypress had shift held" bit is not something
	// evdev has a way to express.
	case SDL_SCANCODE_LSHIFT: return 42;
	case SDL_SCANCODE_RSHIFT: return 54;
	case SDL_SCANCODE_LCTRL:  return 29;
	case SDL_SCANCODE_RCTRL:  return 97;
	case SDL_SCANCODE_LALT:   return 56;
	case SDL_SCANCODE_RALT:   return 100;
	case SDL_SCANCODE_LGUI:   return 125;
	case SDL_SCANCODE_RGUI:   return 126;
	case SDL_SCANCODE_APPLICATION: return 127;
	case SDL_SCANCODE_F1:  return 59;
	case SDL_SCANCODE_F2:  return 60;
	case SDL_SCANCODE_F3:  return 61;
	case SDL_SCANCODE_F4:  return 62;
	case SDL_SCANCODE_F5:  return 63;
	case SDL_SCANCODE_F6:  return 64;
	case SDL_SCANCODE_F7:  return 65;
	case SDL_SCANCODE_F8:  return 66;
	case SDL_SCANCODE_F9:  return 67;
	case SDL_SCANCODE_F10: return 68;
	case SDL_SCANCODE_F11: return 87;
	case SDL_SCANCODE_F12: return 88;
	case SDL_SCANCODE_PRINTSCREEN: return 99;
	case SDL_SCANCODE_SCROLLLOCK:  return 70;
	case SDL_SCANCODE_PAUSE:       return 119;
	case SDL_SCANCODE_INSERT:      return 110;
	case SDL_SCANCODE_HOME:        return 102;
	case SDL_SCANCODE_PAGEUP:      return 104;
	case SDL_SCANCODE_DELETE:      return 111;
	case SDL_SCANCODE_END:         return 107;
	case SDL_SCANCODE_PAGEDOWN:    return 109;
	case SDL_SCANCODE_RIGHT:       return 106;
	case SDL_SCANCODE_LEFT:        return 105;
	case SDL_SCANCODE_DOWN:        return 108;
	case SDL_SCANCODE_UP:          return 103;
	case SDL_SCANCODE_NUMLOCKCLEAR: return 69;
	case SDL_SCANCODE_KP_DIVIDE:   return 98;
	case SDL_SCANCODE_KP_MULTIPLY: return 55;
	case SDL_SCANCODE_KP_MINUS:    return 74;
	case SDL_SCANCODE_KP_PLUS:     return 78;
	case SDL_SCANCODE_KP_ENTER:    return 96;
	case SDL_SCANCODE_KP_1: return 79;
	case SDL_SCANCODE_KP_2: return 80;
	case SDL_SCANCODE_KP_3: return 81;
	case SDL_SCANCODE_KP_4: return 75;
	case SDL_SCANCODE_KP_5: return 76;
	case SDL_SCANCODE_KP_6: return 77;
	case SDL_SCANCODE_KP_7: return 71;
	case SDL_SCANCODE_KP_8: return 72;
	case SDL_SCANCODE_KP_9: return 73;
	case SDL_SCANCODE_KP_0: return 82;
	case SDL_SCANCODE_KP_PERIOD: return 83;
	default: return 0;   // nothing this device claims to have
	}
}

// A keypress as bytes for a *serial* console, which is a different problem
// from the one above.
//
// The UART carries characters, not keys, so the only things translated here
// are the ones that have no character: the arrows and the navigation block
// become the ANSI sequences a terminal would send, and Ctrl+letter becomes
// its control character. Everything printable is deliberately absent --
// SDL_TEXTINPUT delivers that, with the host's layout and any dead-key
// composition already applied, and re-deriving it from keysym plus a shift
// bit is what the previous hand-rolled table did. That table covered a US
// layout and silently produced the wrong punctuation on every other one.
//
// Returns the number of bytes written to `out` (at most 4).
static int console_key_bytes(const RawInputEvent &ev, uint8_t out[4])
{
	const bool ctrl = (ev.mods & KMOD_CTRL) != 0;

	// Ctrl+letter -> 0x01..0x1a. Ctrl+C has to work at a shell prompt and
	// it produces no text-input event, so this is the only path for it.
	if (ctrl && ev.sdl_keysym >= 'a' && ev.sdl_keysym <= 'z') {
		out[0] = (uint8_t)(ev.sdl_keysym - 'a' + 1);
		return 1;
	}
	// The handful of non-letter control characters worth having: Ctrl+[ is
	// escape, Ctrl+\ and Ctrl+] are quit and the telnet escape, Ctrl+space
	// is NUL.
	if (ctrl) {
		switch (ev.sdl_keysym) {
		case SDLK_LEFTBRACKET:  out[0] = 0x1b; return 1;
		case SDLK_BACKSLASH:    out[0] = 0x1c; return 1;
		case SDLK_RIGHTBRACKET: out[0] = 0x1d; return 1;
		case SDLK_SPACE:        out[0] = 0x00; return 1;
		default: break;
		}
	}

	switch (ev.sdl_keysym) {
	// Return sends CR, not LF. The guest's line discipline has ICRNL set
	// and converts it; send LF and the shell is handed a character it does
	// not treat as end-of-line, so the command is typed and never runs.
	case SDLK_RETURN:
	case SDLK_KP_ENTER:  out[0] = '\r'; return 1;
	// DEL rather than BS: that is what a terminal's backspace key sends,
	// and what readline and the kernel's line editor both expect.
	case SDLK_BACKSPACE: out[0] = 0x7f; return 1;
	case SDLK_TAB:       out[0] = '\t'; return 1;
	case SDLK_ESCAPE:    out[0] = 0x1b; return 1;
	default: break;
	}

	// CSI sequences. Three bytes for the cursor keys, four for the
	// navigation block, which is the shape a VT100 and every terminal
	// since has used.
	const char *csi = nullptr;
	switch (ev.sdl_keysym) {
	case SDLK_UP:       csi = "[A"; break;
	case SDLK_DOWN:     csi = "[B"; break;
	case SDLK_RIGHT:    csi = "[C"; break;
	case SDLK_LEFT:     csi = "[D"; break;
	case SDLK_HOME:     csi = "[H"; break;
	case SDLK_END:      csi = "[F"; break;
	case SDLK_INSERT:   csi = "[2~"; break;
	case SDLK_DELETE:   csi = "[3~"; break;
	case SDLK_PAGEUP:   csi = "[5~"; break;
	case SDLK_PAGEDOWN: csi = "[6~"; break;
	default: return 0;
	}
	out[0] = 0x1b;
	int n = 1;
	for (const char *p = csi; *p && n < 4; p++) out[n++] = (uint8_t)*p;
	return n;
}


// Drive the guest's input from a script, so the input devices are testable
// without a window and a person -- the whole chain, from event to virtio
// queue to evdev to a shell to fbcon, ending in a -fbdump you can read.
//
// The format is one command per line, `#` comments and blank lines
// ignored:
//
//   key <code> <0|1>         a key up or down
//   type <text>              that text as press/release pairs, US layout
//   rel <dx> <dy>            relative mouse movement
//   abs <x> <y>              Linux: the pointer to a framebuffer pixel
//   btn <left|middle|right> <0|1>
//   wheel <v>                vertical wheel clicks, positive is up
//   sleep <ms>               wait ms x 10000 instructions
//   wait <n>                 wait n instructions; k, M and G suffixes
//
// `key` is in whichever numbering the guest's keyboard uses: evdev codes
// for a Linux boot (see evdev_keycode), and DOOM's own codes from doomkeys.h
// for a bare-metal one, where 27 is escape and 13 is return. `rel`, `btn`
// and `wheel` likewise go to whichever mouse the mode has.
//
// The script runs on the CPU thread and its clock is the instruction count,
// so the same script delivers the same input at the same instructions on
// every run. `sleep` used to be host milliseconds, which made a script's
// timing depend on how fast the host happened to be; it is now a fixed
// number of instructions per millisecond, close to one host millisecond at
// the interpreter's speed, so existing scripts keep roughly their pacing.
// The script starts at the instruction where -expect matched, if given.
//
// Every command is followed by 30 "ms" and every typed character by 20: the
// guest drains its queues at emulated speed, and a script that fires a
// hundred events at once tests queue overflow rather than what it meant to.

namespace {

// US layout, for `type` only. This is a keyboard emulator's one unavoidable
// layout assumption: the script says "type a", and the only way to turn
// that into a physical key is to pick a layout. The real input path never
// does this -- it forwards scancodes and lets the guest's own keymap decide
// (see evdev_keycode) -- so this table is test scaffolding.
struct Chord { char ch; uint16_t code; bool shift; };
const Chord chords[] = {
	{'a',30,0},{'b',48,0},{'c',46,0},{'d',32,0},{'e',18,0},{'f',33,0},
	{'g',34,0},{'h',35,0},{'i',23,0},{'j',36,0},{'k',37,0},{'l',38,0},
	{'m',50,0},{'n',49,0},{'o',24,0},{'p',25,0},{'q',16,0},{'r',19,0},
	{'s',31,0},{'t',20,0},{'u',22,0},{'v',47,0},{'w',17,0},{'x',45,0},
	{'y',21,0},{'z',44,0},
	{'1',2,0},{'2',3,0},{'3',4,0},{'4',5,0},{'5',6,0},
	{'6',7,0},{'7',8,0},{'8',9,0},{'9',10,0},{'0',11,0},
	{' ',57,0},{'-',12,0},{'=',13,0},{'[',26,0},{']',27,0},
	{';',39,0},{'\'',40,0},{'`',41,0},{'\\',43,0},{',',51,0},
	{'.',52,0},{'/',53,0},{'\n',28,0},
	{'A',30,1},{'B',48,1},{'C',46,1},{'D',32,1},{'E',18,1},{'F',33,1},
	{'G',34,1},{'H',35,1},{'I',23,1},{'J',36,1},{'K',37,1},{'L',38,1},
	{'M',50,1},{'N',49,1},{'O',24,1},{'P',25,1},{'Q',16,1},{'R',19,1},
	{'S',31,1},{'T',20,1},{'U',22,1},{'V',47,1},{'W',17,1},{'X',45,1},
	{'Y',21,1},{'Z',44,1},
	{'!',2,1},{'@',3,1},{'#',4,1},{'$',5,1},{'%',6,1},{'^',7,1},
	{'&',8,1},{'*',9,1},{'(',10,1},{')',11,1},{'_',12,1},{'+',13,1},
	{'{',26,1},{'}',27,1},{':',39,1},{'"',40,1},{'~',41,1},{'|',43,1},
	{'<',51,1},{'>',52,1},{'?',53,1},
};

const char *const input_kind_names[] = { "kbd", "mouse", "uart", "dkey", "dmove", "dbtn" };

// "1500", "250k", "3M", "2G".
uint64_t parse_count(const std::string &s)
{
	char *end = nullptr;
	double v = std::strtod(s.c_str(), &end);
	if (end && *end) {
		if (*end == 'k' || *end == 'K') v *= 1e3;
		else if (*end == 'M') v *= 1e6;
		else if (*end == 'G') v *= 1e9;
	}
	return v > 0 ? (uint64_t)v : 0;
}

} // namespace

bool DoomSystem::set_input_record(const char *path)
{
	record_file = std::fopen(path, "w");
	if (!record_file) return false;
	std::fprintf(record_file, "doomv-input 1\n# instruction kind a b c d\n");
	// The clock's start, which -rtc=host took from the host: an input like
	// any other, so a replay starts the guest's clock where this run did.
	std::fprintf(record_file, "0 rtc %llu\n", (unsigned long long)memory.get_rtc().epoch_seconds());
	std::fflush(record_file);
	return true;
}

bool DoomSystem::set_input_replay(const char *path)
{
	std::ifstream f(path);
	if (!f) return false;
	std::string line;
	while (std::getline(f, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (line.empty() || line[0] == '#' || line.rfind("doomv-input", 0) == 0) continue;
		std::istringstream in(line);
		unsigned long long stamp = 0;
		std::string kind;
		unsigned a = 0, b = 0;
		int c = 0, dd = 0;
		// The real-time clock's start: "0 rtc <seconds since 1970>".
		if (in >> stamp >> kind && kind == "rtc") {
			unsigned long long seconds = 0;
			in >> seconds;
			memory.get_rtc().set_epoch(seconds);
			continue;
		}
		// A sound period: "<instruction> snd tx" (played), or
		// "<instruction> snd rx <hex samples>" (recorded).
		if (kind == "snd") {
			std::string dir, hex;
			in >> dir >> hex;
			SoundEvent e{(uint64_t)stamp, dir == "rx", {}};
			for (size_t i = 0; i + 1 < hex.size(); i += 2)
				e.data.push_back((uint8_t)std::stoul(hex.substr(i, 2), nullptr, 16));
			replay_snd.push_back(std::move(e));
			continue;
		}
		// A network frame: "<instruction> net <hex bytes>".
		if (kind == "net") {
			std::string hex;
			in >> hex;
			std::vector<uint8_t> frame;
			for (size_t i = 0; i + 1 < hex.size(); i += 2)
				frame.push_back((uint8_t)std::stoul(hex.substr(i, 2), nullptr, 16));
			replay_net.push_back({(uint64_t)stamp, std::move(frame)});
			continue;
		}
		in.clear();
		in.seekg(0);
		if (!(in >> stamp >> kind >> a >> b >> c >> dd)) {
			std::cout << "input log: cannot parse '" << line << "'" << std::endl;
			return false;
		}
		GuestInput ev;
		bool known = false;
		for (uint8_t k = 0; k < sizeof(input_kind_names) / sizeof(input_kind_names[0]); k++) {
			if (kind == input_kind_names[k]) { ev.kind = (GuestInput::Kind)k; known = true; }
		}
		if (!known) {
			std::cout << "input log: unknown input '" << kind << "'" << std::endl;
			return false;
		}
		ev.a = (uint16_t)a; ev.b = (uint16_t)b; ev.c = c; ev.d = dd;
		replay_events.push_back({ (uint64_t)stamp, ev });
	}
	replaying = true;
	return true;
}

void DoomSystem::prepare_input()
{
	if (replaying) {
		if (!input_script_path.empty())
			std::cout << "-replay given: ignoring -input=" << input_script_path << std::endl;
		return;
	}

	if (!input_script_path.empty()) {
		std::ifstream f(input_script_path);
		if (!f) {
			std::cout << "cannot open input script: " << input_script_path << std::endl;
		} else {
			std::string line;
			while (std::getline(f, line)) {
				// Tolerate CRLF: a trailing CR turns every argument into a
				// parse error a long way from its cause.
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
				if (line.empty() || line[0] == '#') continue;
				script_lines.push_back(line);
			}
			script_active = true;
		}
	}

	// Stdin redirected from a file is known in full before the guest runs,
	// so it is read now and fed from the CPU thread as the UART has room --
	// deterministic, with nothing to record. A pipe or a terminal is read
	// live by console_stdin_loop.
	if (headless && linux_mode) {
		struct stat st;
		if (fstat(fileno(stdin), &st) == 0 && S_ISREG(st.st_mode)) {
			int c;
			// A pipe carries LF; the kernel's line discipline expects the CR
			// a terminal sends for return.
			while ((c = std::fgetc(stdin)) != EOF) uart_backlog.push_back((uint8_t)(c == '\n' ? '\r' : c));
			stdin_preloaded = true;
		}
	}
}

void DoomSystem::submit_input(const GuestInput &in)
{
	std::lock_guard<std::mutex> lock(input_mutex);
	// A paused machine drains nothing, so this has to stop somewhere.
	if (input_incoming.size() >= (1u << 20)) return;
	input_incoming.push_back(in);
	input_waiting.store(true, std::memory_order_relaxed);
}

void DoomSystem::apply_input(const GuestInput &in)
{
	switch (in.kind) {
	case GuestInput::Kbd:
		memory.get_keyboard().push(in.a, in.b, (uint32_t)in.c);
		break;
	case GuestInput::Mouse:
		memory.get_mouse().push(in.a, in.b, (uint32_t)in.c);
		if (in.a == VirtioInput::EV_ABS) {
			if (in.b == VirtioInput::ABS_X) guest_pointer_x = in.c;
			if (in.b == VirtioInput::ABS_Y) guest_pointer_y = in.c;
		}
		break;
	case GuestInput::Uart:
		memory.get_uart().push_rx((uint8_t)in.a);
		break;
	case GuestInput::DoomKey:
		memory.push_key_event(in.c != 0, (uint8_t)in.a);
		break;
	case GuestInput::DoomMove:
		memory.push_mouse_motion(in.c, in.d);
		break;
	case GuestInput::DoomButton:
		memory.push_mouse_button(in.a, in.c != 0);
		break;
	}
}

void DoomSystem::record_input(uint64_t now, const GuestInput &in)
{
	if (!record_file) return;
	std::fprintf(record_file, "%llu %s %u %u %d %d\n", (unsigned long long)now,
	             input_kind_names[in.kind], (unsigned)in.a, (unsigned)in.b, (int)in.c, (int)in.d);
	record_dirty = true;
}

// Every input the guest can observe -- a virtio key or pointer event, a byte
// for the serial console, a DOOM key or mouse movement -- reaches a device
// here and nowhere else: on the CPU thread, at an instruction count that is
// a multiple of INPUT_PERIOD. The window, stdin and the script only ask.
//
// That is what makes a run reproducible. Where input comes from -- a
// script, a file, a person -- decides *which* inputs there are; this
// decides *when* the guest sees them, and it decides it from the
// instruction count alone. A script or a stdin file therefore produces the
// same run every time. A person or a pipe cannot, because what they send
// and when is outside the machine; -record captures exactly what was
// committed and at which instruction, and -replay commits it again at the
// same instructions, which reproduces the run.
void DoomSystem::service_input(uint64_t now)
{
	if (replaying) {
		while (replay_pos < replay_events.size() && replay_events[replay_pos].first <= now)
			apply_input(replay_events[replay_pos++].second);
		if (input_waiting.load(std::memory_order_relaxed)) {
			std::lock_guard<std::mutex> lock(input_mutex);
			input_incoming.clear();
			input_waiting.store(false, std::memory_order_relaxed);
		}
		service_network(now);
		service_sound(now);
		memory.get_rtc().poll(memory.get_timer().get_mtime(), memory.get_aplic());
		memory.pump_input();
		return;
	}

	run_script(now);
	run_paste(now);

	if (input_waiting.load(std::memory_order_relaxed)) {
		std::vector<GuestInput> batch;
		{
			std::lock_guard<std::mutex> lock(input_mutex);
			batch.swap(input_incoming);
			input_waiting.store(false, std::memory_order_relaxed);
		}
		for (const GuestInput &in : batch) {
			if (in.kind == GuestInput::Uart) uart_backlog.push_back((uint8_t)in.a);
			else commit_input(now, in);
		}
	}

	// Serial bytes wait their turn for room in the 16-byte ring rather than
	// being dropped, and none are sent before -expect has matched. A byte is
	// recorded when it enters the ring, which is the moment the guest can
	// see it.
	if (!uart_backlog.empty() && memory.get_uart().expect_seen()) {
		Uart &uart = memory.get_uart();
		while (!uart_backlog.empty() && uart.try_push_rx(uart_backlog.front())) {
			GuestInput in;
			in.kind = GuestInput::Uart;
			in.a = uart_backlog.front();
			record_input(now, in);
			uart_backlog.pop_front();
		}
	}

	service_network(now);
	service_sound(now);
	memory.get_rtc().poll(memory.get_timer().get_mtime(), memory.get_aplic());
	if (record_dirty) {
		std::fflush(record_file);
		record_dirty = false;
	}
	memory.pump_input();
}

// Network frames for the guest enter here, at the input points, as keys do:
// the network is outside the machine, and this is where its timing is fixed.
// Live, they come from the NAT and are recorded; on a replay, from the log.
void DoomSystem::service_network(uint64_t now)
{
	VirtioNet &net = memory.get_net();
	Aplic &aplic = memory.get_aplic();
	if (replaying) {
		while (replay_net_pos < replay_net.size() && replay_net[replay_net_pos].first <= now)
			net.receive(replay_net[replay_net_pos++].second, memory, aplic);
		return;
	}
	if (!usernet || !usernet->has_frames()) return;
	std::vector<std::vector<uint8_t>> frames;
	usernet->to_guest(frames);
	for (auto &f : frames) {
		record_frame(now, f);
		net.receive(std::move(f), memory, aplic);
	}
}

void DoomSystem::record_frame(uint64_t now, const std::vector<uint8_t> &frame)
{
	if (!record_file) return;
	std::fprintf(record_file, "%llu net ", (unsigned long long)now);
	for (uint8_t b : frame) std::fprintf(record_file, "%02x", b);
	std::fputc('\n', record_file);
	record_dirty = true;
}

void DoomSystem::set_sound()
{
	memory.get_snd().set_enabled(true);
	if (!replaying) host_audio = std::make_unique<HostAudio>();
}

void DoomSystem::record_sound(uint64_t now, bool rx, const std::vector<uint8_t> &data)
{
	if (!record_file) return;
	std::fprintf(record_file, "%llu snd %s", (unsigned long long)now, rx ? "rx " : "tx");
	if (rx)
		for (uint8_t b : data) std::fprintf(record_file, "%02x", b);
	std::fputc('\n', record_file);
	record_dirty = true;
}

// A period of sound finishes here, at an input point, when the host is ready
// for it: played once the host's queue has room -- so the guest is paced by
// the host's sound card, as by real hardware -- and recorded once the host
// has captured as much as the guest's buffer holds. With no host device,
// periods finish at once: played into nothing, or recorded as silence.
void DoomSystem::service_sound(uint64_t now)
{
	VirtioSnd &snd = memory.get_snd();
	if (!snd.is_enabled()) return;
	Aplic &aplic = memory.get_aplic();
	if (replaying) {
		while (replay_snd_pos < replay_snd.size() && replay_snd[replay_snd_pos].at <= now) {
			const SoundEvent &e = replay_snd[replay_snd_pos++];
			if (!e.rx) snd.finish_tx(memory, aplic);
			else if (snd.next_rx_size() == e.data.size()) snd.finish_rx(e.data.data(), memory, aplic);
		}
		return;
	}

	// Output: every period the guest posts goes to the host as soon as the
	// stream runs, and returns to the guest once the host has played past its
	// end -- a period finishes when it has been heard, as on real hardware,
	// not when it was handed over; finishing the whole buffer at once reads
	// to ALSA as an underrun.
	const VirtioSnd::Stream &out = snd.stream(VirtioSnd::OUTPUT);
	const bool host_out = snd.tx_count() && host_audio && host_audio->open_output(out.hz(), out.channels, out.format);
	for (size_t i = 0; host_out && i < snd.tx_count(); i++) {
		if (!snd.tx_mark(i)) snd.tx_mark(i) = host_audio->play(snd.tx_data(i).data(), snd.tx_data(i).size());
	}
	while (snd.tx_count()) {
		if (host_out && snd.tx_mark(0) && snd.tx_mark(0) > host_audio->played()) break;
		snd.finish_tx(memory, aplic);   // played, or no host device to play it
		record_sound(now, false, snd_buf);
	}
	// A stream released while sound was queued -- an aborted player -- stops
	// now, rather than playing out what the guest has thrown away.
	if (host_out) snd_out_live = true;
	if (snd_out_live && (out.state == VirtioSnd::State::Ready || out.state == VirtioSnd::State::Idle)) {
		host_audio->stop_output();
		snd_out_live = false;
	}

	if (!snd.running(VirtioSnd::INPUT)) {
		if (host_audio) host_audio->close_input();
		return;
	}
	while (const uint32_t want = snd.next_rx_size()) {
		const VirtioSnd::Stream &s = snd.stream(VirtioSnd::INPUT);
		snd_buf.assign(want, s.format == VirtioSnd::FMT_U8 ? 0x80 : 0);   // silence
		if (host_audio && host_audio->open_input(s.hz(), s.channels, s.format)) {
			if (host_audio->available() < want) break;
			host_audio->record(snd_buf.data(), want);
		}
		snd.finish_rx(snd_buf.data(), memory, aplic);
		record_sound(now, true, snd_buf);
	}
}

bool DoomSystem::set_network(std::string &error)
{
	VirtioNet &net = memory.get_net();
	net.set_connected(true);
	if (replaying) return true;   // the log's frames, and nothing sent anywhere
	usernet = std::make_unique<UserNet>();
	if (!usernet->start(error)) {
		usernet.reset();
		net.set_connected(false);
		return false;
	}
	UserNet *backend = usernet.get();
	net.on_transmit = [backend](const uint8_t *frame, size_t len) { backend->from_guest(frame, len); };
	return true;
}

void DoomSystem::submit_paste(const std::string &text)
{
	std::lock_guard<std::mutex> lock(paste_mutex);
	// Appended, not replaced: two pastes in quick succession should both
	// arrive, in order, rather than the second cutting the first short.
	// Bounded for the same reason submit_input is -- a paused machine types
	// nothing, and the clipboard can hold a lot.
	if (paste_incoming.size() + text.size() > (1u << 20)) return;
	paste_incoming += text;
}

// Types whatever Ctrl+Alt+V has queued, one character at a time, at the pace
// exec_script_line's `type` uses. Slower than the host can paste on purpose:
// the keystrokes go to a tty or an X client that has to read them, and a
// burst arrives faster than either drains.
//
// Independent of run_script rather than sharing its queue: a script is a
// recorded sequence with its own timing, and a paste landing in the middle of
// one would interleave two texts into the same keyboard. Pasting while a
// script runs simply waits for it.
void DoomSystem::run_paste(uint64_t now)
{
	if (paste_typing.empty()) {
		std::lock_guard<std::mutex> lock(paste_mutex);
		if (paste_incoming.empty()) return;
		paste_typing.swap(paste_incoming);
		paste_pos = 0;
		paste_due = now;
	}
	// A script owns the keyboard while it runs; see above.
	if (script_active) return;
	while (paste_pos < paste_typing.size() && now >= paste_due) {
		type_script_char(now, paste_typing[paste_pos++]);
		paste_due = now + TYPE_GAP;
	}
	if (paste_pos >= paste_typing.size()) {
		paste_typing.clear();
		paste_pos = 0;
	}
}

void DoomSystem::run_script(uint64_t now)
{
	if (!script_active || !memory.get_uart().expect_seen()) return;
	while (script_active && now >= script_due) {
		if (script_type_pos < script_typing.size()) {
			type_script_char(now, script_typing[script_type_pos++]);
			script_due = now + TYPE_GAP;
			if (script_type_pos == script_typing.size()) {
				script_typing.clear();
				script_type_pos = 0;
				script_due += 30 * SCRIPT_INSTR_PER_MS;
			}
			continue;
		}
		if (script_line >= script_lines.size()) {
			script_active = false;
			console_drain();
			std::cout << "input script finished" << std::endl;
			return;
		}
		exec_script_line(now, script_lines[script_line++]);
	}
}

void DoomSystem::type_script_char(uint64_t now, char ch)
{
	GuestInput in;
	if (!linux_mode) {
		// DOOM's key codes are ASCII for everything printable.
		in.kind = GuestInput::DoomKey;
		in.a = (uint8_t)ch;
		in.c = 1;
		commit_input(now, in);
		in.c = 0;
		commit_input(now, in);
		return;
	}
	for (const Chord &chord : chords) {
		if (chord.ch != ch) continue;
		in.kind = GuestInput::Kbd;
		const auto key = [&](uint16_t code, int value) {
			in.a = VirtioInput::EV_KEY; in.b = code; in.c = value;
			commit_input(now, in);
			in.a = VirtioInput::EV_SYN; in.b = VirtioInput::SYN_REPORT; in.c = 0;
			commit_input(now, in);
		};
		if (chord.shift) key(42, 1);
		key(chord.code, 1);
		key(chord.code, 0);
		if (chord.shift) key(42, 0);
		return;
	}
	std::cout << "input script: no key for '" << ch << "'" << std::endl;
}

void DoomSystem::exec_script_line(uint64_t now, const std::string &line)
{
	std::istringstream in(line);
	std::string cmd;
	in >> cmd;
	uint64_t delay = 30 * SCRIPT_INSTR_PER_MS;

	const auto mouse = [&](uint16_t type, uint16_t code, int value) {
		GuestInput ev;
		ev.kind = GuestInput::Mouse; ev.a = type; ev.b = code; ev.c = value;
		commit_input(now, ev);
		ev.a = VirtioInput::EV_SYN; ev.b = VirtioInput::SYN_REPORT; ev.c = 0;
		commit_input(now, ev);
	};

	if (cmd == "sleep") {
		std::string ms;
		in >> ms;
		script_due = now + parse_count(ms) * SCRIPT_INSTR_PER_MS;
		return;
	}
	if (cmd == "wait") {
		std::string n;
		in >> n;
		script_due = now + parse_count(n);
		return;
	}
	if (cmd == "key") {
		int code = 0, val = 0;
		in >> code >> val;
		GuestInput ev;
		if (linux_mode) {
			ev.kind = GuestInput::Kbd; ev.a = VirtioInput::EV_KEY; ev.b = (uint16_t)code; ev.c = val;
			commit_input(now, ev);
			ev.a = VirtioInput::EV_SYN; ev.b = VirtioInput::SYN_REPORT; ev.c = 0;
			commit_input(now, ev);
		} else {
			ev.kind = GuestInput::DoomKey; ev.a = (uint8_t)code; ev.c = val;
			commit_input(now, ev);
		}
	} else if (cmd == "type") {
		// The rest of the line verbatim, spaces included, so `type echo
		// hello` does what it looks like. Typed one character per step of
		// run_script.
		std::string text;
		std::getline(in, text);
		if (!text.empty() && text[0] == ' ') text.erase(0, 1);
		script_typing = text;
		script_type_pos = 0;
		script_due = now;
		return;
	} else if (cmd == "rel") {
		int dx = 0, dy = 0;
		in >> dx >> dy;
		if (linux_mode) {
			// The pointer is absolute, so a relative step is taken from
			// where the committed events left it.
			commit_pointer(now, guest_pointer_x + dx, guest_pointer_y + dy);
		} else {
			GuestInput ev;
			ev.kind = GuestInput::DoomMove; ev.c = dx; ev.d = dy;
			commit_input(now, ev);
		}
	} else if (cmd == "abs") {
		int x = 0, y = 0;
		in >> x >> y;
		if (linux_mode) commit_pointer(now, x, y);
	} else if (cmd == "btn") {
		std::string which;
		int val = 0;
		in >> which >> val;
		if (linux_mode) {
			uint16_t code = VirtioInput::BTN_LEFT;
			if (which == "right")  code = VirtioInput::BTN_RIGHT;
			if (which == "middle") code = VirtioInput::BTN_MIDDLE;
			mouse(VirtioInput::EV_KEY, code, val);
		} else {
			GuestInput ev;
			ev.kind = GuestInput::DoomButton;
			ev.a = (which == "right") ? 1 : (which == "middle") ? 2 : 0;
			ev.c = val;
			commit_input(now, ev);
		}
	} else if (cmd == "wheel") {
		int v = 0;
		in >> v;
		// DOOM has no wheel: doomgeneric's key hook carries no axis for it.
		if (linux_mode) mouse(VirtioInput::EV_REL, VirtioInput::REL_WHEEL, v);
	} else {
		std::cout << "input script: unknown command '" << cmd << "'" << std::endl;
		delay = 0;
	}
	script_due = now + delay;
}

void DoomSystem::run()
{
	// Four threads. The CPU thread executes. The display thread takes whole
	// frames from the guest framebuffer, the dashboard thread draws the
	// register panels from shared_snapshot (published under snapshot_mutex
	// once per burst), and this thread owns the window: it polls input and
	// composites what the other two hand it. Memory/Registers/Debugger stay
	// exclusively CPU-thread-owned; the only other ways in are the input
	// queues, which lock, and the framebuffer bytes the display thread
	// reads, which it checks against a write counter.
	//
	// The guest's display size is given to the window, and one snapshot
	// published, before any of those threads start. Without that the first
	// frames were laid out for DOOM's 320x200 whatever the machine was --
	// a Linux boot came up in the DOOM layout, with zeroed registers and
	// "???" in the trace, until the CPU thread finished its first
	// 200000-instruction burst, a second or more in.
	if (!headless) {
		gui.set_guest_display(linux_mode ? Memory::LFB_W : Memory::FB_W,
		                      linux_mode ? Memory::LFB_H : Memory::FB_H);
		gui.set_absolute_pointer(linux_mode);
	}
	prepare_input();
	publish_snapshot();

	std::thread cpu_thread(&DoomSystem::cpu_loop, this);
	cpu_thread.detach();

	// Refreshed while the machine runs, headless or not: the point of the
	// dump is to see the screen of a guest that is still going.
	if (!fb_dump_path.empty())
		fbdump_thread = std::thread(&DoomSystem::fbdump_loop, this);

	// Headless: nothing to draw and nothing to poll, so this thread just
	// waits for the guest to finish and then ends the process. Without
	// this the run would sit in the loop below forever with no window,
	// which is the worst of both worlds.
	if (headless) {
		// ...except for the console. Without a window there is no keyboard,
		// so a headless Linux boot could be watched but never answered --
		// which leaves anything past a login prompt untestable except by
		// hand, in a window, by a person. stdin covers that gap.
		if (linux_mode && !stdin_preloaded && !replaying)
			std::thread(&DoomSystem::console_stdin_loop, this).detach();
		while (!run_finished) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		stopping = true;
		if (fbdump_thread.joinable()) fbdump_thread.join();
		if (lockstep_active) lockstep_report();
		console_drain();
		std::exit(lockstep_failed ? 1 : 0);
	}

	display_thread = std::thread(&DoomSystem::display_loop, this);
	dashboard_thread = std::thread(&DoomSystem::dashboard_loop, this);

	while (true) {
		for (const RawInputEvent &ev : gui.poll_input()) {
			// The window's own keys, intercepted before either mode
			// forwards anything, so the guest never sees them as input.
			//
			// Ctrl+Alt for the two new ones rather than more function
			// keys. A bare function key is not free: DOOM binds all
			// twelve, so F10 would have been "quit game" and F11 the
			// gamma control, and a guest that has taken the mouse needs a
			// way out that is not also a key it might mean to press.
			// Ctrl+Alt+G for grab is the convention every other emulator
			// uses. F9 stays as it is -- it predates this and is already
			// documented -- and it costs DOOM its quickload.
			if (ev.kind == RawInputEvent::Kind::Key && ev.pressed) {
				const bool ctrl_alt = (ev.mods & KMOD_CTRL) && (ev.mods & KMOD_ALT);
				if (ev.sdl_keysym == SDLK_F9) { resume_requested = true; continue; }
				if (ctrl_alt && ev.sdl_keysym == SDLK_g) {
					gui.set_mouse_captured(!gui.mouse_captured());
					continue;
				}
				// Only in Linux mode, where there is a framebuffer big
				// enough for the question to arise. In DOOM mode this
				// would do nothing and cost the guest a keystroke.
				if (ctrl_alt && ev.sdl_keysym == SDLK_f && linux_mode) {
					gui.toggle_fb_fullscreen();
					continue;
				}
				// Paste the host clipboard by typing it. SDL's clipboard
				// belongs to the thread that set the video mode, which is
				// this one.
				//
				// Two keys for it, Ctrl+Alt+V and F8, because a chord and a
				// function key get taken by different things and having both
				// means one is usually free. F8 costs the guest that key --
				// it is intercepted here and never forwarded -- which is the
				// trade for a paste that needs no modifier to arrive.
				// Linux-only, since pasting into DOOM is not a thing to want,
				// and DOOM binds every function key.
				if ((ctrl_alt && ev.sdl_keysym == SDLK_v)
				    || (linux_mode && ev.sdl_keysym == SDLK_F8)) {
					if (char *text = SDL_GetClipboardText()) {
						submit_paste(text);
						SDL_free(text);
					}
					continue;
				}
			}

			if (!linux_mode) {
				// DOOM needs both key edges: movement is held down rather
				// than typed. The mouse goes into accumulators instead of
				// a queue -- see the MMIO_MOUSE_MOVE comment in memory.hpp
				// for why those are different shapes.
				switch (ev.kind) {
				case RawInputEvent::Kind::Key: {
					GuestInput in;
					in.kind = GuestInput::DoomKey;
					in.a = translate_key(ev.sdl_keysym);
					in.c = ev.pressed ? 1 : 0;
					submit_input(in);
					break;
				}
				case RawInputEvent::Kind::MouseMotion: {
					GuestInput in;
					in.kind = GuestInput::DoomMove;
					in.c = ev.dx;
					in.d = ev.dy;
					submit_input(in);
					break;
				}
				case RawInputEvent::Kind::MouseButton: {
					// DOOM's own bit order, from d_event.h: 0 left,
					// 1 right, 2 middle. Not evdev's, and not SDL's.
					int bit = -1;
					if (ev.button == SDL_BUTTON_LEFT)   bit = 0;
					if (ev.button == SDL_BUTTON_RIGHT)  bit = 1;
					if (ev.button == SDL_BUTTON_MIDDLE) bit = 2;
					if (bit >= 0) {
						GuestInput in;
						in.kind = GuestInput::DoomButton;
						in.a = (uint16_t)bit;
						in.c = ev.pressed ? 1 : 0;
						submit_input(in);
					}
					break;
				}
				default:
					break;
				}
				continue;
			}

			// Linux gets the same event twice over, through two devices
			// that are not alternatives to each other. The virtio
			// keyboard is a real input device, so it drives the
			// framebuffer console, and anything that reads /dev/input; the
			// UART is the serial console on hvc0. Which one a given guest
			// is listening to depends on its own console= setting, and
			// this side has no way to know -- so both are fed, and the one
			// nothing is reading costs a few queued events.
			switch (ev.kind) {
			case RawInputEvent::Kind::Key: {
				// Auto-repeat is the host's, and the guest's input layer
				// does its own from the held state. Forwarding a repeat
				// as a fresh press would make the guest see a key pressed
				// twice without being released, so the keyboard gets only
				// real edges. The serial console has no held state and
				// does want repeats, which is why this is filtered here
				// and not in poll_input.
				if (!ev.repeat) {
					const uint16_t code = evdev_keycode(ev.sdl_scancode);
					if (code) {
						GuestInput in;
						in.kind = GuestInput::Kbd;
						in.a = VirtioInput::EV_KEY; in.b = code; in.c = ev.pressed ? 1 : 0;
						submit_input(in);
						in.a = VirtioInput::EV_SYN; in.b = VirtioInput::SYN_REPORT; in.c = 0;
						submit_input(in);
					}
				}
				if (ev.pressed) {
					uint8_t bytes[4];
					const int n = console_key_bytes(ev, bytes);
					for (int i = 0; i < n; i++) {
						GuestInput in;
						in.kind = GuestInput::Uart;
						in.a = bytes[i];
						submit_input(in);
					}
				}
				break;
			}
			case RawInputEvent::Kind::Text:
				// Printable characters, for the serial console only. The
				// keyboard above already sent the key that produced them.
				for (const char *p = ev.text; *p; p++) {
					GuestInput in;
					in.kind = GuestInput::Uart;
					in.a = (uint8_t)*p;
					submit_input(in);
				}
				break;
			case RawInputEvent::Kind::MouseMotion:
				// Only ever over the display (poll_input drops the rest),
				// already in framebuffer pixels.
				move_pointer(ev.x, ev.y);
				break;
			case RawInputEvent::Kind::MouseButton: {
				// A press happens where the pointer is. Without this it
				// happens wherever the last reported motion left the guest's
				// cursor -- which is not here after a click that brought
				// the window to the front, or anything else that moved the
				// host pointer without a motion event over the display --
				// and a context menu opens somewhere else.
				if (ev.pressed) move_pointer(ev.x, ev.y);
				uint16_t code = 0;
				if (ev.button == SDL_BUTTON_LEFT)   code = VirtioInput::BTN_LEFT;
				if (ev.button == SDL_BUTTON_RIGHT)  code = VirtioInput::BTN_RIGHT;
				if (ev.button == SDL_BUTTON_MIDDLE) code = VirtioInput::BTN_MIDDLE;
				if (code) {
					GuestInput in;
					in.kind = GuestInput::Mouse;
					in.a = VirtioInput::EV_KEY; in.b = code; in.c = ev.pressed ? 1 : 0;
					submit_input(in);
					in.a = VirtioInput::EV_SYN; in.b = VirtioInput::SYN_REPORT; in.c = 0;
					submit_input(in);
				}
				break;
			}
			case RawInputEvent::Kind::MouseWheel: {
				GuestInput in;
				in.kind = GuestInput::Mouse;
				in.a = VirtioInput::EV_REL;
				if (ev.dy) { in.b = VirtioInput::REL_WHEEL;  in.c = ev.dy; submit_input(in); }
				if (ev.dx) { in.b = VirtioInput::REL_HWHEEL; in.c = ev.dx; submit_input(in); }
				in.a = VirtioInput::EV_SYN; in.b = VirtioInput::SYN_REPORT; in.c = 0;
				submit_input(in);
				break;
			}
			}
		}

		// Composite whatever the display and dashboard threads have
		// handed over, and present. VSYNC paces this loop.
		gui.present();

		// Poweroff, specifically -- not run_finished, which is also set by
		// a debugger halt and by a test finishing, both of which want the
		// window to stay up so the dashboard can be read and F9 can resume.
		// A machine that has been switched off is the one case where there
		// is nothing left to look at. Checked after the render so the last
		// frame the guest drew is the one on screen.
		if (memory.poweroff_requested()) break;
	}

	stopping = true;
	display_thread.join();
	dashboard_thread.join();
	if (fbdump_thread.joinable()) fbdump_thread.join();
	console_drain();
}
