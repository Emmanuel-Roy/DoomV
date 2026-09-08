#include "doom_system.hpp"
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
	if (!memory.load_blob(initrd_path, Memory::RAM_BASE + 0x2300000)) return false;

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

void DoomSystem::publish_snapshot()
{
	Snapshot snap;

	const uint32_t *fb32 = reinterpret_cast<const uint32_t *>(memory.framebuffer());
	std::copy(fb32, fb32 + Memory::FB_W * Memory::FB_H, snap.framebuffer.begin());

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
	}
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
	}
}
