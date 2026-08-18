#include "doom_system.hpp"
#include <SDL2/SDL.h>
#include <fstream>
#include <vector>

DoomSystem::DoomSystem() : decoder(core, regs, memory)
{
}

bool DoomSystem::init(const char *wad_path, const char *elf_path, int scale_factor)
{
	if (!gui.init(scale_factor)) return false;

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

uint8_t DoomSystem::translate_key(uint32_t sdl_keysym) const
{
	// Numeric values match doomkeys.h exactly (KEY_RIGHTARROW etc) -- see
	// tools/doombuild/doomgeneric/doomgeneric/doomkeys.h.
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

	uint32_t pc = regs.get_pc();
	if (debugger.should_halt(pc, false)) {
		debugger.dump_log(regs, "crash.log");
		return;
	}

	uint32_t instr = memory.read32(pc);
	DispatchResult result = decoder.decode_and_dispatch(instr);
	regs.record_history(pc, instr, result.mnemonic);

	if (debugger.should_halt(pc, result.illegal)) {
		debugger.dump_log(regs, "crash.log");
	}

	memory.step_instructions(1);
}

void DoomSystem::run()
{
	while (true) {
		for (int i = 0; i < 20000; i++) {
			if (!debugger.halted) step();
		}

		for (const RawKeyEvent &ev : gui.poll_input()) {
			memory.push_key_event(ev.pressed, translate_key(ev.sdl_keysym));
		}

		gui.render(regs, memory, debugger);
	}
}
