#include "doom_system.hpp"
#include <iostream>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>

DoomSystem::DoomSystem() : decoder(core, regs, memory)
{
}

bool DoomSystem::attach_disk(const std::string &path)
{
	return memory.get_disk().open(path, /*read_only=*/false);
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

uint8_t DoomSystem::translate_console_key(uint32_t sdl_keysym) const
{
	switch (sdl_keysym) {
	case SDLK_RETURN:    return '\r';
	case SDLK_BACKSPACE: return 0x7f;
	case SDLK_TAB:       return '\t';
	case SDLK_ESCAPE:    return 0x1b;
	default: break;
	}

	// SDL_Keycode is already ASCII for unshifted printable keys (a-z as
	// lowercase, digits, space, and most punctuation) -- shift only needs
	// handling for letters (case) and the punctuation keys whose shifted
	// glyph isn't just "the next character along".
	bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
	if (sdl_keysym >= 'a' && sdl_keysym <= 'z') {
		return shift ? (uint8_t)(sdl_keysym - 'a' + 'A') : (uint8_t)sdl_keysym;
	}
	if (shift) {
		switch (sdl_keysym) {
		case '1': return '!';
		case '2': return '@';
		case '3': return '#';
		case '4': return '$';
		case '5': return '%';
		case '6': return '^';
		case '7': return '&';
		case '8': return '*';
		case '9': return '(';
		case '0': return ')';
		case '-': return '_';
		case '=': return '+';
		case '[': return '{';
		case ']': return '}';
		case '\\': return '|';
		case ';': return ':';
		case '\'': return '"';
		case ',': return '<';
		case '.': return '>';
		case '/': return '?';
		case '`': return '~';
		default: break;
		}
	}

	if (sdl_keysym >= 0x20 && sdl_keysym < 0x7f) return (uint8_t)sdl_keysym;
	return 0;
}

void DoomSystem::step()
{
	if (debugger.halted) return;

	if (core.check_and_take_interrupt(regs, memory)) {
		// pc has already been redirected into the trap handler -- this
		// "step" was the interrupt itself, not whatever instruction was
		// about to execute at the old pc.
		memory.step_instructions(1);
		return;
	}

	uint64_t pc = regs.get_pc();
	if (debugger.should_halt(pc, false)) {
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
	uint64_t fetch_paddr;
	if (!core.translate_or_trap(regs, memory, pc, AccessType::Fetch, fetch_paddr, 2)) {
		// A page fault redirected pc into the trap handler already --
		// nothing more to do for this step.
		memory.step_instructions(1);
		return;
	}
	uint32_t instr = memory.read16(fetch_paddr);
	if ((instr & 0x3) == 0x3) {
		uint64_t hi_paddr;
		if (!core.translate_or_trap(regs, memory, pc + 2, AccessType::Fetch, hi_paddr, 2)) {
			memory.step_instructions(1);
			return;
		}
		instr |= (uint32_t)memory.read16(hi_paddr) << 16;
	}
	DispatchResult result = decoder.decode_and_dispatch(pc, instr);
	// A compressed instruction's raw fetch also contains the next
	// instruction's bytes in its upper half -- mask those off so the
	// trace log/crash dump show just the actual 16-bit encoding.
	uint32_t recorded_instr = (result.decoded.length == 2) ? (instr & 0xFFFF) : instr;
	regs.record_history(pc, recorded_instr, result.decoded);

	// A test that signals completion through HTIF stops here, with its
	// signature dumped exactly as a breakpoint would. This is what lets a
	// suite that exports only `tohost` -- no `pass` label to break on -- be
	// run at all.
	if (memory.tohost_written()) {
		debugger.halted = true;
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

	if (debugger.should_halt(pc, result.illegal)) {
		if (result.illegal) {
			// Remember what to raise on resume. recorded_instr is the
			// encoding as actually fetched (masked to 16 bits for a
			// compressed one), which is what the spec wants in [ms]tval.
			pending_illegal = true;
			pending_illegal_tval = recorded_instr;
		}
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
	std::ofstream f(fb_dump_path, std::ios::binary);
	if (!f.is_open()) return;
	f << "P6\n" << Memory::LFB_W << " " << Memory::LFB_H << " " << 255 << "\n";
	const uint32_t *px = reinterpret_cast<const uint32_t *>(memory.linux_framebuffer());
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
	// Refresh the framebuffer dump periodically, not only when the run
	// stops. A Linux guest reaches a login prompt and then sits there
	// forever, so "when it stops" never arrives -- and the whole point of
	// the dump is to be able to check what is on the screen of a machine
	// that is still running. Every 200th publish is a few seconds apart and
	// costs one 2MB write.
	if (!fb_dump_path.empty() && ++fb_dump_tick % 200 == 0) dump_framebuffer();

	Snapshot snap;

	// Two framebuffers, one window. DOOM's is the 320x200 buffer
	// doomgeneric hands us through MMIO_FB; Linux's is the 1024x768 linear
	// aperture at LFB_BASE that simple-framebuffer writes. Which one is
	// live is decided by how the machine was started, not by which has been
	// written -- an unwritten framebuffer is black, and a black screen is a
	// legitimate thing for a Linux guest to be showing before fbcon takes
	// over.
	if (linux_mode) {
		snap.fb_w = Memory::LFB_W;
		snap.fb_h = Memory::LFB_H;
		snap.framebuffer.resize((size_t)Memory::LFB_W * Memory::LFB_H);
		const uint32_t *lfb32 = reinterpret_cast<const uint32_t *>(memory.linux_framebuffer());
		std::copy(lfb32, lfb32 + (size_t)Memory::LFB_W * Memory::LFB_H, snap.framebuffer.begin());
	} else {
		const uint32_t *fb32 = reinterpret_cast<const uint32_t *>(memory.framebuffer());
		std::copy(fb32, fb32 + Memory::FB_W * Memory::FB_H, snap.framebuffer.begin());
	}

	for (int i = 0; i < 32; i++) snap.x[i] = regs.read_x(i);
	for (int i = 0; i < 32; i++) std::memcpy(&snap.v_lo[i], regs.read_v(i), sizeof(uint64_t));
	snap.pc = regs.get_pc();
	snap.halted = debugger.halted;

	int active_idx = (regs.history_pos() + Registers::HISTORY_SIZE - 1) % Registers::HISTORY_SIZE;
	snap.active = regs.history_at(active_idx);
	for (int i = 0; i < 13; i++) {
		int pos = (regs.history_pos() + i) % Registers::HISTORY_SIZE;
		snap.trace[i] = regs.history_at(pos);
	}

	snap.csr_count = regs.csr_history_count();
	for (int i = 0; i < snap.csr_count; i++) {
		uint16_t addr = regs.csr_history_at(i);
		snap.csrs[i] = { addr, core.read_csr_effective(regs, memory, addr) };
	}

	std::lock_guard<std::mutex> lock(snapshot_mutex);
	shared_snapshot = std::move(snap);
}

void DoomSystem::resume_from_halt()
{
	uint64_t pc = regs.get_pc();
	if (pending_illegal) {
		pending_illegal = false;
		core.raise_illegal_instruction(regs, pending_illegal_tval);
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
			step();
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
			std::cout << "guest requested poweroff" << std::endl;
			run_finished = true;
			return;
		}
	}
}

// Host stdin -> the guest's UART receive ring, for headless runs.
//
// The window's keyboard path (translate_console_key, in run()) is the only
// way anything ever reached the guest console, which meant a -ng boot was
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
	// Nothing is sent until the guest has printed whatever -expect asked
	// for. Without a needle this is already true and the loop starts
	// immediately, which is right for a guest that is waiting at a prompt
	// before the emulator even starts reading stdin.
	while (!run_finished && !memory.get_uart().expect_seen())
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	int c;
	while (!run_finished && (c = std::fgetc(stdin)) != EOF) {
		const uint8_t byte = (c == '\n') ? (uint8_t)'\r' : (uint8_t)c;
		while (!run_finished && !memory.get_uart().try_push_rx(byte))
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// Deliberately does not end the run on EOF. A script that pipes in a few
	// commands and closes stdin still wants the guest to keep going and keep
	// printing; the run ends when the guest ends it, or when whoever started
	// it does.
}

void DoomSystem::run()
{
	// CPU execution and rendering run on separate threads: instruction
	// bursts no longer stall input polling/rendering, and vice versa. The
	// two sides only ever communicate through shared_snapshot (a full copy
	// under snapshot_mutex, published once per burst) and Memory's key
	// queue (locked separately) -- everything else in Memory/Registers/
	// Debugger stays exclusively CPU-thread-owned, so it needs no locking.
	std::thread cpu_thread(&DoomSystem::cpu_loop, this);
	cpu_thread.detach();

	// Headless: nothing to draw and nothing to poll, so this thread just
	// waits for the guest to finish and then ends the process. Without
	// this the run would sit in the loop below forever with no window,
	// which is the worst of both worlds.
	if (headless) {
		// ...except for the console. Without a window there is no keyboard,
		// so a headless Linux boot could be watched but never answered --
		// which leaves anything past a login prompt untestable except by
		// hand, in a window, by a person. stdin covers that gap.
		if (linux_mode) std::thread(&DoomSystem::console_stdin_loop, this).detach();
		while (!run_finished) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::exit(0);
	}

	while (true) {
		for (const RawKeyEvent &ev : gui.poll_input()) {
			// Intercepted before either mode forwards anything to the guest,
			// so the resume key is never seen as Doom input or console input.
			if (ev.pressed && ev.sdl_keysym == SDLK_F9) {
				resume_requested = true;
				continue;
			}
			if (linux_mode) {
				// One byte per keypress, not per press+release -- unlike
				// Doom's own key_queue (which needs up/down edges for
				// movement), a console only ever wants the character once.
				if (!ev.pressed) continue;
				uint8_t ch = translate_console_key(ev.sdl_keysym);
				if (ch != 0) memory.get_uart().push_rx(ch);
			} else {
				memory.push_key_event(ev.pressed, translate_key(ev.sdl_keysym));
			}
		}

		Snapshot snap;
		{
			std::lock_guard<std::mutex> lock(snapshot_mutex);
			snap = shared_snapshot;
		}
		gui.render(snap);

		// Poweroff, specifically -- not run_finished, which is also set by
		// a debugger halt and by a test finishing, both of which want the
		// window to stay up so the dashboard can be read and F9 can resume.
		// A machine that has been switched off is the one case where there
		// is nothing left to look at. Checked after the render so the last
		// frame the guest drew is the one on screen.
		if (memory.poweroff_requested()) break;
	}
}
