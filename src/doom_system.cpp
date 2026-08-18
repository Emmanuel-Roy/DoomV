#include "doom_system.hpp"
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

	return true;
}

uint8_t DoomSystem::translate_key(uint32_t sdl_keysym) const
{
	(void)sdl_keysym;
	// TODO
	return 0;
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
	regs.record_history(pc, instr);

	DispatchResult result = decoder.decode_and_dispatch(instr);

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
