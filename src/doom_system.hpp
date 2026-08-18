#pragma once
#include "memory.hpp"
#include "registers.hpp"
#include "riscv_core.hpp"
#include "riscv_decoder.hpp"
#include "debugger.hpp"
#include "gui.hpp"

class DoomSystem {
public:
	DoomSystem();

	bool init(const char *wad_path, const char *elf_path, int scale_factor);
	void run();
	void step();

private:
	Memory memory;
	Registers regs;
	RiscvCore core;
	Decoder decoder;
	Debugger debugger;
	Gui gui;

	// TODO: SDL keysym -> Doom key code translation, see PLAN.md's
	// "input key mapping" item. Currently drops all key events.
	uint8_t translate_key(uint32_t sdl_keysym) const;
};
