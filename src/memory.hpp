#pragma once
#include "timer.hpp"
#include "imsic.hpp"
#include "aplic.hpp"
#include "uart.hpp"
#include "virtio_blk.hpp"
#include "virtio_input.hpp"
#include <cstdint>
#include <mutex>
#include <vector>

class Memory {
public:
	// MMIO/RAM/WAD addresses are still small values that fit comfortably in
	// 64 bits -- RV64 doesn't require actually using a 64-bit-wide address
	// space, just being able to represent one.
	static constexpr uint64_t MMIO_INPUT = 0x10000000;
	static constexpr uint64_t MMIO_TICK  = 0x10000004;
	static constexpr uint64_t MMIO_DEBUG = 0x10000008;
	static constexpr uint64_t MMIO_FB    = 0x10001000;

	// DOOM's mouse. A different shape from the Linux guest's, on purpose.
	//
	// Linux gets a virtio-input device and an event stream, because that is
	// what an input device is. DOOM asks a different question: I_ReadMouse
	// runs once a frame and wants "how far has it moved since I last
	// asked", so this is a pair of accumulators and a button mask rather
	// than a queue. The difference matters in the failure case -- if a
	// frame is slow, movement should *merge* into the next reading rather
	// than queue up behind it, which is right for a pointer and wrong for
	// keys. It is also why the key queue next door is a queue.
	//
	// MMIO_MOUSE_MOVE: dx in bits 31-16, dy in 15-0, both signed 16-bit.
	//                  Reading is destructive -- it returns the movement
	//                  since the last read and zeroes the accumulator.
	// MMIO_MOUSE_BTN:  bit 0 left, bit 1 right, bit 2 middle, held state.
	//                  Reading does not clear it; a held button is a state
	//                  and not an event. The bit order is DOOM's own (see
	//                  d_event.h's ev_mouse), which is not evdev's.
	static constexpr uint64_t MMIO_MOUSE_MOVE = 0x1000000C;
	static constexpr uint64_t MMIO_MOUSE_BTN  = 0x10000010;

	// Where the WAD is and how big it is, published rather than agreed.
	//
	// The guest used to hardcode its own copy of WAD_BASE, and the two
	// drifted: RAM_SIZE grew from 256MB to 1GB so that Ubuntu would fit,
	// WAD_BASE is RAM_BASE + RAM_SIZE, and the guest went on reading
	// 0x90000000. DOOM then reported "Wad file doom1.wad doesn't have IWAD
	// or PWAD id" -- it was reading a gigabyte short of the WAD and finding
	// zeros. Nothing caught it because no test suite runs DOOM.
	//
	// A constant that has to be written down identically in two places
	// will eventually be written down differently, and neither side can
	// static_assert against the other across the emulation boundary. So
	// the guest asks instead. Publishing the length too means one guest
	// binary works with any WAD, where before it was compiled with the
	// size of the WAD it was built alongside.
	static constexpr uint64_t MMIO_WAD_BASE = 0x10000014;
	static constexpr uint64_t MMIO_WAD_SIZE = 0x10000018;

	// Stage 4's UART -- clear of MMIO_INPUT/TICK/DEBUG above (which end at
	// 0x1000000B) and MMIO_FB below.
	static constexpr uint64_t UART_BASE = 0x10000100;
	static constexpr uint64_t UART_SIZE = 0x100;

	static constexpr int FB_W = 320;
	static constexpr int FB_H = 200;
	static constexpr uint32_t FB_SIZE = FB_W * FB_H * 4; // 32bpp, matches doomgeneric's native output

	// The Linux framebuffer, which is a different device from DOOM's above
	// and deliberately not the same memory. DOOM writes its 320x200 through
	// MMIO_FB because doomgeneric hands us exactly that buffer; Linux wants
	// a linear aperture it can ioremap and a resolution worth looking at,
	// and the two cannot share a base because MMIO_FB's 256KB would have to
	// grow across the virtio window at 0x10008000.
	//
	// 0x50000000 is clear of everything: RAM starts at 0x80000000, the
	// IMSIC files sit at 0x24/0x28000000, and the platform devices are all
	// under 0x11000000. Being *outside* the device tree's memory node is
	// what keeps Linux from allocating over it -- no reserved-memory entry
	// needed, which is how a carved-out framebuffer aperture works on real
	// hardware too.
	//
	// The geometry is a constant rather than something the guest selects.
	// simple-framebuffer has no mode-setting protocol at all: the driver
	// reads width, height, stride and format out of the device tree and
	// trusts them, so these three numbers and the framebuffer@ node in
	// tools/linux/dts/doomv.dts have to agree exactly. They are checked
	// against each other by a static_assert on the size below and by
	// nothing at all on the width, so changing one means changing both.
	static constexpr uint64_t LFB_BASE = 0x50000000;
	static constexpr int LFB_W = 1024;
	static constexpr int LFB_H = 768;
	static constexpr uint32_t LFB_STRIDE = LFB_W * 4;
	static constexpr uint64_t LFB_SIZE = (uint64_t)LFB_STRIDE * LFB_H;

	// sifive,test0 -- the "test finisher". One 32-bit register: 0x5555
	// powers off, 0x7777 reboots, 0x3333 fails with a code in the high
	// half. It exists because OpenSBI's generic platform implements SBI
	// SRST by looking for exactly this device in the device tree, and
	// without it a guest calling `poweroff` gets "not supported" and spins.
	// A machine that cannot be shut down by the thing running on it cannot
	// be scripted, which matters for a rootfs build that has to hand
	// control back when it finishes.
	static constexpr uint64_t TEST_BASE = 0x00100000;
	static constexpr uint64_t TEST_SIZE = 0x1000;

	// RAM_BASE moved from the original 0x10041000 for Stage 3: OpenSBI's
	// `generic` platform build hardcodes its own load/entry address
	// (FW_TEXT_START, see tools/linux/opensbi/build.sh) to 0x80000000 and
	// requires it 2MB-aligned -- 0x10041000 wasn't, and rather than
	// override+rebuild OpenSBI to match some other aligned address,
	// matching OpenSBI's own already-built default is strictly simpler
	// (real hardware/QEMU-virt convention too). RAM_SIZE grew from 16MB
	// since even just the DTB load offset OpenSBI expects
	// (FW_TEXT_START + 0x2200000) is ~34MB in, before the ~23MB kernel
	// Image or any of Linux's own runtime allocation.
	static constexpr uint64_t RAM_BASE  = 0x80000000;
	// 1GB. 256MB was ample for a busybox initramfs and is not enough for a
	// distribution: an Ubuntu rootfs wants room for the kernel, the page
	// cache it reads through, and systemd's own working set, and the
	// failure mode of being short is an OOM kill of init rather than
	// anything that names memory as the problem.
	static constexpr uint64_t RAM_SIZE  = 1024ull * 1024 * 1024;
	static constexpr uint64_t WAD_BASE  = RAM_BASE + RAM_SIZE;
	static constexpr uint64_t WAD_SIZE  = 20 * 1024 * 1024;

	// Platform devices added for Stage 2 (timer + AIA interrupt
	// controller) -- addresses follow common real-hardware/QEMU-virt
	// convention (helps Stage 3's device tree resemble known-good
	// examples), and don't collide with anything above.
	static constexpr uint64_t CLINT_BASE = 0x02000000;
	static constexpr uint64_t CLINT_SIZE = 0x10000;
	static constexpr uint64_t APLIC_BASE = 0x0C000000;
	static constexpr uint64_t APLIC_SIZE = 0x4000;
	static constexpr uint64_t IMSIC_M_BASE = 0x24000000;
	static constexpr uint64_t IMSIC_S_BASE = 0x28000000;
	static constexpr uint64_t IMSIC_SIZE   = 0x1000;

	// virtio over MMIO. One slot is enough for a root disk; the address
	// follows the QEMU-virt convention so a device tree written against
	// that layout needs no adjusting, and so does anyone reading it.
	static constexpr uint64_t VIRTIO_BASE = 0x10008000;
	static constexpr uint64_t VIRTIO_SIZE = 0x1000;

	// Two more virtio slots, for a keyboard and a mouse. Separate devices
	// rather than one: the driver registers one input device per virtio
	// device, and a single device claiming both keys and relative motion
	// would be classified as a mouse with a hundred buttons by everything
	// that looks at it -- the VT layer included.
	//
	// Not the next slots up from the disk, which is where they were put
	// first and is inside DOOM's framebuffer. MMIO_FB is 320*200*4 bytes
	// long, so its aperture runs 0x10001000..0x1003F7FF and covers every
	// QEMU-convention virtio slot there is -- 0x10008000 included.
	//
	// The disk survives that overlap only because read32/write32 happen to
	// test VIRTIO_BASE before MMIO_FB. read8/write8 test MMIO_FB first, and
	// virtio-input is the first device here whose config space is read a
	// byte at a time: the driver's reads came back as framebuffer bytes,
	// which are zeros, so both devices probed, registered, and reported no
	// name and no capabilities. A device that is there and answers nothing
	// is a worse failure than one that is not there at all.
	//
	// So these sit past the aperture instead of relying on the order of a
	// branch chain. Each still needs its own APLIC source: an MMIO virtio
	// device has exactly one interrupt and there is no way to share it.
	static constexpr uint64_t VIRTIO_KBD_BASE   = 0x10100000;
	static constexpr uint64_t VIRTIO_MOUSE_BASE = 0x10101000;
	static_assert(VIRTIO_KBD_BASE >= MMIO_FB + FB_SIZE,
	              "input devices must not sit inside DOOM's framebuffer aperture");

	Memory();

	uint8_t  read8(uint64_t addr);
	uint16_t read16(uint64_t addr);
	uint32_t read32(uint64_t addr);
	uint64_t read64(uint64_t addr);
	void     write8(uint64_t addr, uint8_t val);
	void     write32(uint64_t addr, uint32_t val);
	void     write64(uint64_t addr, uint64_t val);

	bool load_elf(const char *path);

	// For anything that isn't an ELF -- the Linux kernel's `Image` (a raw
	// flat binary) and a compiled device tree blob. Reads the whole file
	// and copies it in starting at `addr` (physical), bounds-checked
	// against the RAM+WAD span the same way load_elf's segments are.
	bool load_blob(const char *path, uint64_t addr);

	// Copies wad_bytes into the WAD region at WAD_BASE.
	bool load_wad(const uint8_t *wad_bytes, size_t len);

	void push_key_event(bool pressed, uint8_t doom_keycode);
	// DOOM's mouse, from the window thread. `doom_bit` is DOOM's own
	// numbering: 0 left, 1 right, 2 middle.
	void push_mouse_motion(int dx, int dy);
	void push_mouse_button(int doom_bit, bool pressed);

	// Instructions-per-ms calibrated to observed throughput: DoomSystem
	// bursts 20000 instructions per render at ~60fps (~16.7ms/frame), so
	// roughly 1200 instructions per "ms" tracks close to real 35Hz tic
	// pacing. A naive 1:1 mapping (1 instruction = 1ms) made simulated
	// time race ~1000x too fast, leaving I_GetTime()-driven game logic
	// stuck trying to process an enormous backlog of tics it thought had
	// already elapsed -- see PLAN.md's debugging notes.
	// Physical memory attributes: whether anything is actually there.
	//
	// An access to an address no device or RAM answers is an access fault,
	// not a silent zero. Reads used to return zero and writes used to
	// vanish, which is the most forgiving possible behaviour and hides
	// exactly the bugs this matters for -- a wild pointer, or a page table
	// pointing somewhere that does not exist, both simply appeared to work.
	//
	// riscv-arch-test probes this deliberately: RVMODEL_ACCESS_FAULT_ADDRESS
	// is physical address 0, and a dozen tests map a valid, permissive PTE
	// onto it and require a store access fault.
	//
	// The whole access has to be inside one region; an access straddling the
	// end of RAM is a fault even though its first byte is fine.
	// HTIF tohost: every RISC-V bare-metal test suite signals completion by
	// storing to a symbol called `tohost` -- 1 for pass, (code<<1)|1 for a
	// failing test number. DoomV had no way to notice, so a test could only
	// be stopped by breaking on an address read out of its symbol table,
	// which works for suites that export a `pass` label and not for the
	// ones that export only `tohost`.
	//
	// Watching the address makes every suite runnable the same way, and is
	// how the reference models do it.
	void watch_tohost(uint64_t addr) { tohost_addr = addr; tohost_value = 0; }
	void check_tohost();
	uint64_t tohost_written() const { return tohost_value; }

	bool is_backed(uint64_t addr, unsigned size) const;

	uint64_t tohost_addr = 0;
	uint64_t tohost_value = 0;
	bool htif_busy = false;   // see Memory::check_tohost

	static constexpr uint32_t INSTR_PER_MS = 1200;
	void step_instructions(uint32_t count);

	const uint8_t *framebuffer() const { return fb.data(); }
	const uint8_t *linux_framebuffer() const { return lfb.data(); }

	// Set by a guest write to the sifive,test0 register; polled by the
	// render thread, which owns process exit.
	bool poweroff_requested() const { return poweroff; }

	// The two input devices, for the window's event handling to push into.
	VirtioInput &get_keyboard() { return kbd_dev; }
	VirtioInput &get_mouse() { return mouse_dev; }
	// Hand any queued input events to the guest. Called from the CPU
	// thread, which is the only thread allowed to touch guest memory and
	// the queues; the window only ever enqueues.
	void pump_input();
	// Whether that is worth doing at all -- a lock-free check, so the
	// caller can afford to ask often.
	bool input_pending() const
	{
		return kbd_dev.has_pending() || mouse_dev.has_pending();
	}

	// Returns the number of FB writes since the last call, and resets
	// the counter. Used for the doom_fps dashboard metric.
	uint32_t take_fb_write_count();

	// Exposed for ext_zicsr.cpp's mip/mie/miselect-mireg CSR handling and
	// RiscvCore::check_and_take_interrupt -- CSR logic needs to read the
	// timer/IMSIC state that backs the computed MTIP/STIP/MEIP/SEIP bits.
	Timer &get_timer() { return timer; }
	Imsic &get_imsic_m() { return imsic_m; }
	Imsic &get_imsic_s() { return imsic_s; }

	// Exposed for DoomSystem's console-key input routing (Stage 4) to push
	// typed bytes into the UART's RX ring.
	Uart &get_uart() { return uart; }
	VirtioBlk &get_disk() { return disk; }

private:
	// RAM and WAD are contiguous (RAM_BASE..RAM_BASE+RAM_SIZE == WAD_BASE),
	// so one backing buffer covers both -- see the memory map in PLAN.md.
	std::vector<uint8_t> ram;
	std::vector<uint8_t> fb;
	std::vector<uint8_t> lfb;
	bool poweroff = false;

	VirtioInput kbd_dev{VirtioInput::Kind::Keyboard, 2};
	VirtioInput mouse_dev{VirtioInput::Kind::Mouse, 3};

	// Pushed by the render thread (input polling lives there, tied to the
	// SDL window), popped by the CPU thread on MMIO_INPUT reads -- the one
	// piece of Memory state actually touched from both threads.
	std::mutex key_mutex;
	// Accumulated since DOOM last read them, under their own lock: the
	// window thread adds, the CPU thread takes and zeroes.
	std::mutex mouse_mutex;
	int mouse_dx = 0, mouse_dy = 0;
	uint32_t mouse_buttons = 0;

	// Bytes actually loaded, for MMIO_WAD_SIZE.
	uint32_t wad_len = 0;

	uint32_t key_queue[16];
	int key_queue_head;
	int key_queue_tail;

	uint32_t instr_count;   // raw executed-instruction count
	uint32_t tick_counter;  // instr_count / INSTR_PER_MS -- what MMIO_TICK exposes
	uint32_t ms_accum;      // instructions banked toward the next tick_counter++ (avoids a divide every instruction)
	uint32_t fb_write_count;

	VirtioBlk disk;

	// Declaration order matters here: aplic's constructor takes a
	// reference to imsic_s, so imsic_s must finish constructing first --
	// C++ initializes members in declaration order regardless of the
	// initializer-list order in memory.cpp.
	Timer timer;
	Imsic imsic_m;
	Imsic imsic_s;
	Aplic aplic;
	Uart uart;
};
