#pragma once
#include "memory.hpp"
#include "registers.hpp"
#include "riscv_core.hpp"
#include "riscv_decoder.hpp"
#include "debugger.hpp"
#include "gui.hpp"
#include "controls.hpp"
#include "snapshot.hpp"
#include <mutex>

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

	// CPU execution runs on its own thread so rendering isn't blocked on
	// (or blocking) instruction bursts. Only this thread ever touches
	// memory/regs/core/decoder/debugger directly; it hands the render
	// thread a Snapshot copy after every burst instead.
	void cpu_loop();
	void publish_snapshot();
	std::mutex snapshot_mutex;
	Snapshot shared_snapshot;
};
