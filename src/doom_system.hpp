#pragma once
#include <atomic>
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

	// Alternative boot path (Stage 3): OpenSBI + Linux kernel + device
	// tree instead of Doom's own guest ELF. Loads fw_jump.elf via the
	// existing load_elf (works unmodified once RAM_BASE matches its
	// FW_TEXT_START -- see src/memory.hpp), the kernel Image, the
	// initramfs, and the compiled DTB via load_blob at the offsets
	// fw_jump.elf/the DTB itself were built expecting (FW_JUMP_ADDR /
	// FW_JUMP_FDT_ADDR, see tools/linux/opensbi/build.sh, and
	// tools/linux/rootfs/README.md for the initrd offset), then sets up the
	// M-mode entry state the RISC-V SBI/Linux boot protocol requires: pc
	// at RAM_BASE, a0=hart id, a1=DTB pointer.
	bool init_linux_boot(const char *sbi_path, const char *kernel_path, const char *dtb_path, const char *initrd_path);

	void run();
	void step();

	// Pause-then-trap. An illegal instruction freezes the machine so its
	// state can be read off the dashboard; pressing the resume key then
	// delivers the trap the guest would really have taken. Set from the
	// input thread, consumed by the CPU thread -- atomic because those are
	// the only two threads and this is the only thing they share besides
	// the snapshot and the key queue.
	std::atomic<bool> resume_requested{false};
	void watch_tohost(uint64_t addr);

	// Headless: no SDL window, and the process exits as soon as the guest
	// stops rather than sitting in a render loop nobody is watching. A
	// conformance run has no use for a window, and the window is the only
	// reason a finished test has to be killed from outside.
	void set_headless() { headless = true; }
	// Hold the headless stdin feed until the guest's console has printed
	// this string. See console_stdin_loop.
	void set_console_expect(const char *needle) { memory.get_uart().expect(needle); }
	void set_input_script(const char *path) { input_script_path = path; }
	void set_canvas_dump(const char *path) { gui.set_canvas_dump(path); }

	// Attach a raw image as the virtio-blk backing store. Returns false if
	// it cannot be opened, which main reports rather than booting a machine
	// whose disk silently reads as zeros -- a failure that surfaces much
	// later as an unbootable filesystem.
	bool attach_disk(const std::string &path);

	bool headless = false;
	// Set by the CPU thread once a run has stopped *and* its crash log and
	// signature are on disk. debugger.halted is not a substitute: it is
	// raised inside should_halt(), before either file is written, so a
	// headless exit keyed off it truncates the signature it was run to
	// produce. That cost 89 of 663 arch-tests.
	std::atomic<bool> run_finished{false};
	bool pending_illegal = false;
	uint64_t pending_illegal_tval = 0;
	void resume_from_halt();

	// Test/debug hook: halt (and dump full register state via the
	// debugger's crash-log path) as soon as PC reaches `addr`, instead of
	// only on an illegal instruction. Used for comparing a run against a
	// reference simulator at a known point, rather than relying on an
	// executed instruction to trigger the halt.
	void add_breakpoint(uint64_t addr) { debugger.add_breakpoint(addr); }

	// Paired with add_breakpoint: when set, halting also dumps [begin, end)
	// to `path` via Debugger::dump_signature, matching riscv-arch-test's
	// signature-region convention for comparing against a reference sim.
	// Where to write the Linux framebuffer when the run stops, as a PPM.
	//
	// This exists because screenshotting the window turned out not to be a
	// usable way to check whether the framebuffer works. CopyFromScreen
	// grabs whatever is on screen at the window's coordinates -- another
	// window, if this one is not on top, and SetForegroundWindow is
	// routinely refused to a background process. PrintWindow captures the
	// window's own device context but comes back black for GPU-composited
	// SDL content. Both failure modes look exactly like "the framebuffer is
	// empty", which is how an afternoon went into the wrong hypothesis.
	//
	// Dumping the pixels the emulator actually holds settles it with no
	// window involved, and works headless.
	void set_fb_dump(const char *path) { fb_dump_path = path; }
	void dump_framebuffer();

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
	std::string fb_dump_path;
	unsigned fb_dump_tick = 0;

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

	// True from init_linux_boot, false from init -- selects which of
	// translate_key the input-polling loop in
	// run() feeds SDL key events through.
	bool linux_mode = false;

	// SDL keysym + current shift state -> raw ASCII byte, for typing at
	// the Linux-boot console (UART RX). Best-effort: covers normal
	// command typing (letters/digits/space/enter/backspace/tab/common
	// QWERTY-shifted punctuation), not a full keyboard-layout engine.
	// Headless stands in for the keyboard with stdin -- see the comment at
	// the definition for why a guest console needs to be reachable from a
	// pipe at all.
	void console_stdin_loop();
	// Drive the virtio keyboard and mouse from a script, so they are
	// testable without a window and a person. See the definition.
	void replay_input_script();
	std::string input_script_path;

	// CPU execution runs on its own thread so rendering isn't blocked on
	// (or blocking) instruction bursts. Only this thread ever touches
	// memory/regs/core/decoder/debugger directly; it hands the render
	// thread a Snapshot copy after every burst instead.
	void cpu_loop();
	void publish_snapshot();
	std::mutex snapshot_mutex;
	Snapshot shared_snapshot;
};
