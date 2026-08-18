#pragma once
#include "memory.hpp"
#include "registers.hpp"
#include "riscv_core.hpp"
#include "riscv_decoder.hpp"
#include "debugger.hpp"
#include "gui.hpp"
#include "controls.hpp"

class DoomSystem {
public:
	DoomSystem();

	bool init(const char *wad_path, const char *elf_path);
	void run();
	void step();

private:
	Memory memory;
	Registers regs;
	RiscvCore core;
	Decoder decoder;
	Debugger debugger;
	Gui gui;
	ControlMap controls;

	// SDL keysym -> Doom key code. Movement (WASD) comes from
	// controls.json via ControlMap; everything else is fixed here, with
	// printable-ASCII passthrough as the final fallback.
	uint8_t translate_key(uint32_t sdl_keysym) const;
};
