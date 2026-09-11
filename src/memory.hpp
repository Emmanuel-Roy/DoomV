#pragma once
#include "timer.hpp"
#include "imsic.hpp"
#include "aplic.hpp"
#include "uart.hpp"
#include "virtio_blk.hpp"
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

	// Pushed by the render thread (input polling lives there, tied to the
	// SDL window), popped by the CPU thread on MMIO_INPUT reads -- the one
	// piece of Memory state actually touched from both threads.
	std::mutex key_mutex;
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
