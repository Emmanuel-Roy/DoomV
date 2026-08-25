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
#include <string>

class DoomSystem {
public:
	DoomSystem();

	bool init(const char *wad_path, const char *elf_path);
	void run();
	void step();

	// Test/debug hook: halt (and dump full register state via the
	// debugger's crash-log path) as soon as PC reaches `addr`, instead of
	// only on an illegal instruction. Used for comparing a run against a
	// reference simulator at a known point, rather than relying on an
	// executed instruction to trigger the halt.
	void add_breakpoint(uint64_t addr) { debugger.add_breakpoint(addr); }

	// Paired with add_breakpoint: when set, halting also dumps [begin, end)
	// to `path` via Debugger::dump_signature, matching riscv-arch-test's
	// signature-region convention for comparing against a reference sim.
	void set_signature_range(uint64_t begin, uint64_t end, const char *path)
	{
		sig_begin = begin;
		sig_end = end;
		sig_path = path;
		has_sig_range = true;
	}

private:
	bool has_sig_range = false;
	uint64_t sig_begin = 0, sig_end = 0;
	std::string sig_path;

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
