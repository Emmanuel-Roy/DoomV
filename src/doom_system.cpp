#include "doom_system.hpp"
#include <iostream>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
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
			// Deliver queued input part-way through the burst, not just
			// between bursts. A burst is about 30ms of wall time, and a
			// mouse sampled at 30Hz is a mouse that feels broken; this
			// lands around 1.6kHz instead. pump_input is two relaxed
			// atomic loads when there is nothing queued, which is almost
			// always, so the hot path pays for a branch and no more.
			if ((i & 0xFFF) == 0xFFF) memory.pump_input();
		}
		memory.pump_input();
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


// Replay a script of input events into the keyboard and mouse.
//
// This exists because the devices are otherwise untestable. Their events
// come from SDL, so exercising them needs a window, and a window needs a
// person -- which means "the keyboard works" would be a claim resting on
// someone having typed at it once and not on anything reproducible. With
// this, the whole chain is checkable from a script: event to virtio queue
// to evdev to the VT layer to a shell to fbcon to the framebuffer, ending
// in a -fbdump you can read.
//
// The format is one command per line, `#` comments and blank lines
// ignored:
//
//   key <code> <0|1>         a key up or down
//   type <text>              that text as press/release pairs, US layout
//   rel <dx> <dy>            relative mouse movement
//   btn <left|middle|right> <0|1>
//   wheel <v>                vertical wheel clicks, positive is up
//   sleep <ms>               host milliseconds, not guest
//
// `key` is in whichever numbering the guest's keyboard uses, because there
// are two and neither is a superset of the other: evdev codes for a Linux
// boot (see evdev_keycode), and DOOM's own codes from doomkeys.h for a
// bare-metal one, where 27 is escape and 13 is return. `rel`, `btn` and
// `wheel` likewise go to whichever mouse the mode has -- virtio-input for
// Linux, the MMIO accumulators for DOOM.
//
// Every command syncs, and there is a small delay after each one. Both are
// deliberate: an input report is only delivered at a SYN, and the guest
// drains its queue at emulated speed, so a script that fires a hundred
// events with no pacing tests the ring's overflow behaviour rather than
// the thing it meant to test.
void DoomSystem::replay_input_script()
{
	// Wait for the guest to be somewhere that can receive input, the same
	// gate the stdin feed uses and for the same reason: keys delivered
	// before a tty exists go nowhere.
	while (!run_finished && !memory.get_uart().expect_seen())
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	std::ifstream f(input_script_path);
	if (!f) {
		std::cout << "cannot open input script: " << input_script_path << std::endl;
		return;
	}

	// US layout, for `type` only. This is a keyboard emulator's one
	// unavoidable layout assumption: the script says "type a", and the
	// only way to turn that into a physical key is to pick a layout. The
	// real input path never does this -- it forwards scancodes and lets
	// the guest's own keymap decide (see evdev_keycode) -- so this table
	// is test scaffolding and not part of how the device works.
	struct Chord { char ch; uint16_t code; bool shift; };
	static const Chord chords[] = {
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

	const auto tap = [&](uint16_t code, bool shift) {
		VirtioInput &kbd = memory.get_keyboard();
		if (shift) { kbd.push(VirtioInput::EV_KEY, 42, 1); kbd.sync(); }
		kbd.push(VirtioInput::EV_KEY, code, 1);
		kbd.sync();
		kbd.push(VirtioInput::EV_KEY, code, 0);
		kbd.sync();
		if (shift) { kbd.push(VirtioInput::EV_KEY, 42, 0); kbd.sync(); }
	};

	std::string line;
	while (!run_finished && std::getline(f, line)) {
		// Tolerate CRLF: this file is as likely to have been written on
		// Windows as not, and a trailing CR turns every argument into a
		// parse error a long way from its cause.
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();
		if (line.empty() || line[0] == '#') continue;

		std::istringstream in(line);
		std::string cmd;
		in >> cmd;

		if (cmd == "sleep") {
			int ms = 0;
			in >> ms;
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			continue;
		}
		if (cmd == "key") {
			int code = 0, val = 0;
			in >> code >> val;
			if (linux_mode) {
				VirtioInput &kbd = memory.get_keyboard();
				kbd.push(VirtioInput::EV_KEY, (uint16_t)code, (uint32_t)val);
				kbd.sync();
			} else {
				memory.push_key_event(val != 0, (uint8_t)code);
			}
		} else if (cmd == "type") {
			// The rest of the line verbatim, spaces included, so `type
			// echo hello` does what it looks like.
			std::string text;
			std::getline(in, text);
			if (!text.empty() && text[0] == ' ') text.erase(0, 1);
			for (char ch : text) {
				if (!linux_mode) {
					// DOOM's key codes are ASCII for everything
					// printable, so there is no table to consult.
					memory.push_key_event(true, (uint8_t)ch);
					memory.push_key_event(false, (uint8_t)ch);
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					continue;
				}
				bool found = false;
				for (const Chord &c : chords) {
					if (c.ch != ch) continue;
					tap(c.code, c.shift);
					found = true;
					break;
				}
				if (!found)
					std::cout << "input script: no key for '" << ch << "'" << std::endl;
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
		} else if (cmd == "rel") {
			int dx = 0, dy = 0;
			in >> dx >> dy;
			if (!linux_mode) {
				memory.push_mouse_motion(dx, dy);
			} else {
				VirtioInput &ms = memory.get_mouse();
				if (dx) ms.push(VirtioInput::EV_REL, VirtioInput::REL_X, (uint32_t)dx);
				if (dy) ms.push(VirtioInput::EV_REL, VirtioInput::REL_Y, (uint32_t)dy);
				ms.sync();
			}
		} else if (cmd == "btn") {
			std::string which;
			int val = 0;
			in >> which >> val;
			if (!linux_mode) {
				int bit = 0;
				if (which == "right")  bit = 1;
				if (which == "middle") bit = 2;
				memory.push_mouse_button(bit, val != 0);
			} else {
				uint16_t code = VirtioInput::BTN_LEFT;
				if (which == "right")  code = VirtioInput::BTN_RIGHT;
				if (which == "middle") code = VirtioInput::BTN_MIDDLE;
				VirtioInput &ms = memory.get_mouse();
				ms.push(VirtioInput::EV_KEY, code, (uint32_t)val);
				ms.sync();
			}
		} else if (cmd == "wheel") {
			int v = 0;
			in >> v;
			// DOOM has no wheel: doomgeneric's key hook carries no axis
			// for it and DOOM itself predates the hardware.
			if (linux_mode) {
				VirtioInput &ms = memory.get_mouse();
				ms.push(VirtioInput::EV_REL, VirtioInput::REL_WHEEL, (uint32_t)v);
				ms.sync();
			}
		} else {
			std::cout << "input script: unknown command '" << cmd << "'" << std::endl;
			continue;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
	}
	std::cout << "input script finished" << std::endl;
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

	// A scripted input replay runs in either mode and whether or not there
	// is a window: the point of it is to exercise the input path without a
	// person, and DOOM needs that as much as Linux does -- more, since
	// checking DOOM's mouse means watching the rendered view change.
	if (!input_script_path.empty())
		std::thread(&DoomSystem::replay_input_script, this).detach();

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
				case RawInputEvent::Kind::Key:
					memory.push_key_event(ev.pressed, translate_key(ev.sdl_keysym));
					break;
				case RawInputEvent::Kind::MouseMotion:
					memory.push_mouse_motion(ev.dx, ev.dy);
					break;
				case RawInputEvent::Kind::MouseButton: {
					// DOOM's own bit order, from d_event.h: 0 left,
					// 1 right, 2 middle. Not evdev's, and not SDL's.
					int bit = -1;
					if (ev.button == SDL_BUTTON_LEFT)   bit = 0;
					if (ev.button == SDL_BUTTON_RIGHT)  bit = 1;
					if (ev.button == SDL_BUTTON_MIDDLE) bit = 2;
					memory.push_mouse_button(bit, ev.pressed);
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
						VirtioInput &kbd = memory.get_keyboard();
						kbd.push(VirtioInput::EV_KEY, code, ev.pressed ? 1 : 0);
						kbd.sync();
					}
				}
				if (ev.pressed) {
					uint8_t bytes[4];
					const int n = console_key_bytes(ev, bytes);
					for (int i = 0; i < n; i++) memory.get_uart().push_rx(bytes[i]);
				}
				break;
			}
			case RawInputEvent::Kind::Text:
				// Printable characters, for the serial console only. The
				// keyboard above already sent the key that produced them.
				for (const char *p = ev.text; *p; p++)
					memory.get_uart().push_rx((uint8_t)*p);
				break;
			case RawInputEvent::Kind::MouseMotion: {
				VirtioInput &ms = memory.get_mouse();
				if (ev.dx) ms.push(VirtioInput::EV_REL, VirtioInput::REL_X, (uint32_t)ev.dx);
				if (ev.dy) ms.push(VirtioInput::EV_REL, VirtioInput::REL_Y, (uint32_t)ev.dy);
				// One SYN for the pair: a report is a complete state
				// change, and splitting X from Y makes a diagonal
				// movement arrive as two separate steps.
				ms.sync();
				break;
			}
			case RawInputEvent::Kind::MouseButton: {
				uint16_t code = 0;
				if (ev.button == SDL_BUTTON_LEFT)   code = VirtioInput::BTN_LEFT;
				if (ev.button == SDL_BUTTON_RIGHT)  code = VirtioInput::BTN_RIGHT;
				if (ev.button == SDL_BUTTON_MIDDLE) code = VirtioInput::BTN_MIDDLE;
				if (code) {
					VirtioInput &ms = memory.get_mouse();
					ms.push(VirtioInput::EV_KEY, code, ev.pressed ? 1 : 0);
					ms.sync();
				}
				break;
			}
			case RawInputEvent::Kind::MouseWheel: {
				VirtioInput &ms = memory.get_mouse();
				if (ev.dy) ms.push(VirtioInput::EV_REL, VirtioInput::REL_WHEEL, (uint32_t)ev.dy);
				if (ev.dx) ms.push(VirtioInput::EV_REL, VirtioInput::REL_HWHEEL, (uint32_t)ev.dx);
				ms.sync();
				break;
			}
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
