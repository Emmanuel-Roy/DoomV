#pragma once
#include <atomic>
#include <cstdint>
#include "memory.hpp"
#include "registers.hpp"
#include "riscv_core.hpp"
#include "riscv_decoder.hpp"
#include "debugger.hpp"
#include "gui.hpp"
#include "controls.hpp"
#include "snapshot.hpp"
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
	// Stop after exactly `n` instructions and write the machine state to
	// crash.log. The machine is deterministic: the same guest with the same
	// inputs must produce the same file on every run -- registers, CSRs,
	// instruction count and the last 4096 instructions -- whatever the
	// host's threads were doing meanwhile. This is how that is checked.
	void set_stop_at(uint64_t n) { stop_at = n; }

	// Headless: no SDL window, and the process exits as soon as the guest
	// stops rather than sitting in a render loop nobody is watching. A
	// conformance run has no use for a window, and the window is the only
	// reason a finished test has to be killed from outside.
	void set_headless() { headless = true; }
	// Hold the headless stdin feed until the guest's console has printed
	// this string. See console_stdin_loop.
	void set_console_expect(const char *needle) { memory.get_uart().expect(needle); }
	void set_input_script(const char *path) { input_script_path = path; }
	// Input logs. -record writes every input the guest receives with the
	// instruction count it was delivered at; -replay delivers a log's input
	// at exactly those counts and ignores the window, stdin and -input.
	bool set_input_record(const char *path);
	bool set_input_replay(const char *path);
	// -trace: a commit log of every instruction and trap, in Spike's format.
	// -lockstep: run against a reference's commit log and halt at the first
	// record that does not match. See lockstep.cpp.
	bool set_trace(const char *path);
	bool set_lockstep(const char *path);
	// Strict: nothing is taken from the reference. Interrupts are DoomV's own
	// and must land where the reference's did; time, counter, interrupt-state
	// and device reads are compared like everything else. This is the mode
	// that says DoomV matches the reference deterministically.
	void set_lockstep_strict() { lockstep_strict = true; }
	void set_canvas_dump(const char *path) { gui.set_canvas_dump(path); }

	// Attach a raw image as the virtio-blk backing store. Returns false if
	// it cannot be opened, which main reports rather than booting a machine
	// whose disk silently reads as zeros -- a failure that surfaces much
	// later as an unbootable filesystem.
	bool attach_disk(const std::string &path);
	// Attach every *.img in `dir`, in name order, to the storage drive
	// slots. `skip` is the root disk's path, so an image that is already
	// the root is not attached twice. Prints what it attached; a missing
	// directory is not an error, since the folder is optional.
	void attach_drives(const std::string &dir, const std::string &skip);
	// Serve `dir` to the guest as the shared folder, mount tag "shared". A
	// directory that does not exist is skipped quietly, like drives/.
	void attach_shared(const std::string &dir);

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
	// Guest input, delivered deterministically. See service_input.
	struct GuestInput {
		enum Kind : uint8_t {
			Kbd,         // virtio keyboard: a = type, b = code, c = value
			Mouse,       // virtio mouse:    a = type, b = code, c = value
			Uart,        // a = byte for the serial console
			DoomKey,     // a = doomkeys.h code, c = pressed
			DoomMove,    // c = dx, d = dy
			DoomButton,  // a = DOOM button bit, c = pressed
		};
		Kind kind = Kbd;
		uint16_t a = 0, b = 0;
		int32_t c = 0, d = 0;
	};
	// Input is committed at instruction counts that are multiples of this.
	static constexpr uint64_t INPUT_PERIOD = 4096;
	// An input script's `sleep` unit: instructions per millisecond, about
	// one host millisecond at the interpreter's speed.
	static constexpr uint64_t SCRIPT_INSTR_PER_MS = 10000;

	void submit_input(const GuestInput &in);           // any thread
	void service_input(uint64_t now);                  // CPU thread
	void apply_input(const GuestInput &in);
	void record_input(uint64_t now, const GuestInput &in);
	void commit_input(uint64_t now, const GuestInput &in) { apply_input(in); record_input(now, in); }
	void prepare_input();

	std::mutex input_mutex;
	std::vector<GuestInput> input_incoming;            // submitted, not yet committed
	std::atomic<bool> input_waiting{false};
	std::deque<uint8_t> uart_backlog;                  // CPU thread
	bool stdin_preloaded = false;

	std::FILE *record_file = nullptr;
	bool record_dirty = false;
	std::vector<std::pair<uint64_t, GuestInput>> replay_events;
	size_t replay_pos = 0;
	bool replaying = false;

	// The -input script, run by the CPU thread. See run_script.
	std::string input_script_path;
	std::vector<std::string> script_lines;
	size_t script_line = 0;
	uint64_t script_due = 0;
	bool script_active = false;
	std::string script_typing;
	size_t script_type_pos = 0;
	void run_script(uint64_t now);
	void exec_script_line(uint64_t now, const std::string &line);
	void type_script_char(uint64_t now, char ch);

	// One step without the clock, and the clock after it. step() is both;
	// traced_step runs them apart, to see a step's CSR writes before its
	// clock moves -- Sail logs a CSR write before its clock ticks.
	void step_execute();
	void end_step();
	// Sail's clock: mtime advances once every INSNS_PER_TICK steps, and on
	// every tick of a wait, which lasts at most MAX_WAIT_TICKS.
	static constexpr uint32_t INSNS_PER_TICK = 2;   // rva23s64.json platform.instructions_per_tick
	static constexpr uint32_t MAX_WAIT_TICKS = 10;  // rva23s64.json platform.max_time_to_wait
	uint32_t tick_phase = 0;
	void clock_tick();
	void run_wait();

	// Whether check_and_take_interrupt could find anything it did not find
	// the last time it looked. See interrupt_may_be_due.
	uint64_t irq_key = ~0ull;
	uint64_t irq_deadline = 0;
	bool interrupt_may_be_due();
	// mcountinhibit and the Smcntrpmf filters for the mode the hart is in,
	// decided once per change of CSRs or privilege rather than every step.
	uint64_t counter_key = ~0ull;
	bool counts_instret = false, counts_cycle = false;
	void refresh_counter_enables();

	// Instruction fetch, a halfword at a time, with a small cache of code
	// pages that are plain RAM and fetchable as a whole. See fetch16.
	struct FetchPage {
		uint64_t vpage = ~0ull;
		uint64_t key = ~0ull;
		const uint8_t *host = nullptr;
	};
	static constexpr unsigned FETCH_CACHE_SIZE = 256;
	FetchPage fetch_cache[FETCH_CACHE_SIZE];
	bool fetch16(uint64_t vaddr, uint16_t &out);
	// A whole instruction, in one cache lookup where that is exactly
	// equivalent to the two fetch16s it replaces. See fetch_instr.
	bool fetch_instr(uint64_t vaddr, uint32_t &out);

	// lockstep.cpp
	void traced_step();
	void lockstep_end();
	void lockstep_report();
	bool tracing = false;          // -trace or -lockstep: steps go through traced_step
	bool lockstep_active = false;
	bool lockstep_failed = false;
	bool lockstep_strict = false;
	std::FILE *trace_file = nullptr;
	struct LockstepState;
	LockstepState *lock = nullptr;
	// Set by step(): whether the instruction it ran committed, and what it was.
	bool step_committed = false;
	bool step_decoded = false;     // it got as far as decoding an instruction
	uint32_t step_insn = 0;
	uint8_t step_insn_len = 4;

	// CPU execution runs on its own thread so the window isn't blocked on
	// (or blocking) instruction bursts. Only this thread ever touches
	// memory/regs/core/decoder/debugger directly; it hands the dashboard
	// thread a register Snapshot after every burst instead. Pixels go the
	// other way round: the display thread reads the framebuffer itself.
	void cpu_loop();
	void publish_snapshot();
	std::mutex snapshot_mutex;
	Snapshot shared_snapshot;
	uint64_t snapshot_seq = 0;   // CPU thread only
	uint64_t stop_at = 0;
	void stop_at_limit();

	// The periodic -fbdump refresh, on a thread of its own. The dumps taken
	// when a run stops stay on the CPU thread, which has nothing left to do.
	void fbdump_loop();
	void write_framebuffer_dump(const uint32_t *px);
	std::thread fbdump_thread;
	std::mutex fb_dump_mutex;

	// The window's two producers, each on a thread of its own. The display
	// thread takes whole frames out of the guest framebuffer; the dashboard
	// thread draws the panels from shared_snapshot. See their definitions.
	void display_loop();
	void dashboard_loop();
	std::thread display_thread, dashboard_thread;
	std::atomic<bool> stopping{false};

	// Where the committed ABS events have left the guest's pointer, in
	// framebuffer pixels -- what a script's `rel` steps from. CPU thread.
	int guest_pointer_x = Memory::LFB_W / 2, guest_pointer_y = Memory::LFB_H / 2;
	// The window's pointer: submits an absolute move.
	void move_pointer(int x, int y);
	void commit_pointer(uint64_t now, int x, int y);
};
