#include "doom_system.hpp"
#include "pmp.hpp"
#include "mmu.hpp"
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
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
#include <sys/stat.h>

DoomSystem::DoomSystem() : decoder(core, regs, memory)
{
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

	regs.set_pc(Memory::RAM_BASE); // matches _start's placement, see riscv.lds

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
	// FW_TEXT_START + 0x200000, FW_JUMP_FDT_ADDR = FW_TEXT_START + 0x2200000.
	// The initrd offset (+0x2300000) is DoomV's own choice, not something
	// fw_jump.elf cares about -- it just needs to sit past the DTB with
	// headroom and match the DTB's own linux,initrd-start (see
	// tools/linux/rootfs/README.md).
	if (!memory.load_blob(kernel_path, Memory::RAM_BASE + 0x200000)) return false;
	if (!memory.load_blob(dtb_path, Memory::RAM_BASE + 0x2200000)) return false;
	// Optional now: with a virtio disk attached the kernel mounts a real
	// root filesystem instead, and there is no initramfs to place.
	if (initrd_path && initrd_path[0]
	    && !memory.load_blob(initrd_path, Memory::RAM_BASE + 0x2300000))
		return false;

	regs.set_pc(Memory::RAM_BASE);
	regs.write_x(10, 0);                                // a0: hart id
	regs.write_x(11, Memory::RAM_BASE + 0x2200000);      // a1: DTB pointer, SBI/Linux boot convention

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
	counter_key = regs.state_gen + ExtensionsEpoch;
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
	Timer &timer = memory.get_timer();
	const uint64_t key = regs.state_gen + memory.get_imsic_m().generation()
	                   + memory.get_imsic_s().generation() + timer.cmp_generation() + ExtensionsEpoch;
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
	if (regs.state_gen + ExtensionsEpoch != counter_key) refresh_counter_enables();
	if (counts_cycle) regs.bump_csr(0xB00);
	memory.tick_clock();
}

// After a step: minstret if the instruction completed and counts, and the
// clock every INSNS_PER_TICK steps.
void DoomSystem::end_step()
{
	if (step_committed && regs.minstret_increment) regs.bump_csr(0xB02);
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
	const RiscvCore::Wait kind = core.wait_request;
	core.wait_request = RiscvCore::Wait::None;

	constexpr uint64_t TW = 1ull << 21, VTW = 1ull << 21;
	uint32_t remaining = MAX_WAIT_TICKS;
	clock_tick();
	int trap = 0;   // 2 illegal, 22 virtual instruction
	for (;;) {
		const bool timed_out = remaining == 0;
		if (regs.state_gen + ExtensionsEpoch != counter_key) refresh_counter_enables();
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
	const uint64_t key = regs.state_gen + mmu_tlb_generation() + ExtensionsEpoch;
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

void DoomSystem::step_execute()
{
	if (debugger.halted) return;
	const uint64_t traps_before = core.trap_count;
	step_committed = false;
	step_decoded = false;
	if (regs.state_gen + ExtensionsEpoch != counter_key) refresh_counter_enables();
	regs.minstret_increment = counts_instret;

	// In lenient lock-step the reference decides when an interrupt is taken
	// (see traced_step), so the machine's own devices never interrupt by
	// themselves. Strict lock-step takes them as ever, and checks the timing.
	// The same test interrupt_may_be_due opens with, inline: on most steps
	// it is all there is.
	const bool irq_unchanged = regs.state_gen + memory.get_imsic_m().generation()
	                           + memory.get_imsic_s().generation()
	                           + memory.get_timer().cmp_generation() + ExtensionsEpoch == irq_key
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

	uint16_t half;
	if (!fetch16(pc, half)) {
		// A page fault redirected pc into the trap handler already --
		// nothing more to do for this step.
		memory.step_instructions(1);
		return;
	}
	uint32_t instr = half;
	if ((instr & 0x3) == 0x3) {
		if (!fetch16(pc + 2, half)) {
			memory.step_instructions(1);
			return;
		}
		instr |= (uint32_t)half << 16;
	}
	step_decoded = true;
	DispatchResult result = decoder.decode_and_dispatch(pc, instr);
	// A compressed instruction's raw fetch also contains the next
	// instruction's bytes in its upper half -- mask those off so the
	// trace log/crash dump show just the actual 16-bit encoding.
	uint32_t recorded_instr = (result.decoded.length == 2) ? (instr & 0xFFFF) : instr;
	step_insn = recorded_instr;
	step_insn_len = result.decoded.length;
	// Committed: it ran to completion -- not illegal, and no trap taken while
	// it executed, such as a page fault or an ecall.
	step_committed = !result.illegal && core.trap_count == traps_before;
	if (core.wait_request != RiscvCore::Wait::None) run_wait();
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
	// Exact, not approximate: every step, trap or instruction, advances the
	// step count by one, so the first check to see stop_at is the one
	// straight after that instruction.
	const uint64_t n = memory.instruction_count();
	stop_at = 0;
	debugger.halted = true;
	console_drain();
		debugger.dump_log(regs, memory, "crash.log");
	std::cout << "stopped after instruction " << n << "; state in crash.log" << std::endl;
	run_finished = true;
}

void DoomSystem::resume_from_halt()
{
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
	while (true) {
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
		for (int i = 0; i < 200000; i++) {
			if (debugger.halted) break;
			if (tracing) traced_step();
			else step();
			// Input is committed at instruction counts, never at a point
			// in a burst: the burst boundaries depend on halts and resumes,
			// and an input delivered at "whenever the host got to it" is
			// the one thing that made two runs of the same guest differ.
			// Every 4096 instructions is about 2.5kHz at the interpreter's
			// speed, so a mouse still feels attached.
			const uint64_t now = memory.instruction_count();
			if ((now & (INPUT_PERIOD - 1)) == 0) service_input(now);
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
		memory.pump_input();
		return;
	}

	run_script(now);

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

	if (record_dirty) {
		std::fflush(record_file);
		record_dirty = false;
	}
	memory.pump_input();
}

void DoomSystem::run_script(uint64_t now)
{
	if (!script_active || !memory.get_uart().expect_seen()) return;
	while (script_active && now >= script_due) {
		if (script_type_pos < script_typing.size()) {
			type_script_char(now, script_typing[script_type_pos++]);
			script_due = now + 20 * SCRIPT_INSTR_PER_MS;
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
