#pragma once

#include <stdint.h>

// Guest-side memory map. Must match the host's Memory bus dispatch.
//
// Reading MMIO_INPUT is destructive (pops one queued key event, 0 if
// none pending) -- the event queue itself lives host-side in Memory,
// populated each frame from Gui's input polling. Layout: bits 15-8 are
// pressed(1)/released(0), bits 7-0 are the Doom key code. The SDL
// keysym -> Doom key code translation also happens host-side, so this
// file only ever sees already-translated key codes.
#define MMIO_INPUT   0x10000000u

// Mouse. Two registers, and a different shape from MMIO_INPUT above on
// purpose: DOOM reads the mouse once a frame and wants "how far since I
// last asked", so these are accumulators rather than a queue. If a frame
// runs long the movement merges into the next reading instead of queueing
// behind it, which is right for a pointer and wrong for keys.
//
// MMIO_MOUSE_MOVE is destructive to read: it hands back the movement since
// the previous read and zeroes the accumulator. dx is bits 31-16, dy bits
// 15-0, both signed 16-bit. MMIO_MOUSE_BTN is the held button state and is
// not cleared by reading -- bit 0 left, bit 1 right, bit 2 middle, which is
// DOOM's own ev_mouse ordering rather than any hardware's.
#define MMIO_MOUSE_MOVE 0x1000000Cu
#define MMIO_MOUSE_BTN  0x10000010u

// Instruction-count-based tick register (see PLAN.md timing model) --
// advances with executed instructions, not wall-clock time.
#define MMIO_TICK    0x10000004u

// Debug output: each byte written here gets echoed to the host's stdout.
// No real UART protocol, just a write-only character sink -- exists so
// printf/I_Error output isn't silently swallowed by nosys.specs's
// always-fail _write stub.
#define MMIO_DEBUG   0x10000008u

// Framebuffer: native 32bpp RGBA, DOOMGENERIC_RESX * DOOMGENERIC_RESY
// words, doomgeneric already does the palette conversion internally.
// 320*200*4 = 256000 bytes, padded to a 256K page.
#define MMIO_FB      0x10001000u

// RAM (.text/.data/.bss/heap/stack) -- see riscv.lds. Moved to 0x80000000
// for Stage 3 to match OpenSBI's own hardcoded, 2MB-aligned load address
// (Memory::RAM_BASE, src/memory.hpp); no longer adjacent to the MMIO
// region below it. 256MB region, WAD sits right after it ends.
#define RAM_BASE     0x80000000u
#define RAM_SIZE     0x10000000u   // 256MB

// WAD blob, placed by the host loader and read directly by
// w_file_doomv.c. Its address and length are *asked for* rather than
// written down here, because writing them down here is what broke DOOM:
// the host's WAD_BASE is RAM_BASE + RAM_SIZE, RAM_SIZE grew from 256MB to
// 1GB so a distribution would fit, and this file went on saying
// 0x90000000. The symptom was "Wad file doom1.wad doesn't have IWAD or
// PWAD id" -- reading zeros a gigabyte below the WAD -- and no suite runs
// DOOM, so nothing noticed.
//
// Nothing can static_assert across the emulation boundary, so the only
// durable fix is to stop duplicating the constant. These two registers are
// read-only and answer from src/memory.hpp's own values.
#define MMIO_WAD_BASE 0x10000014u
#define MMIO_WAD_SIZE 0x10000018u

// Read once and cached: these never change during a run, and the callers
// include a bump allocator and a read() shim where an MMIO load per call
// would be a waste. Static per translation unit, which is fine -- the cost
// of the duplication is one extra MMIO read per file, not a correctness
// question, and the alternative is a .c file for two accessors.
static inline uint32_t doomv_wad_base(void)
{
	static uint32_t cached;
	if (cached == 0) cached = *(volatile uint32_t *)MMIO_WAD_BASE;
	return cached;
}

static inline uint32_t doomv_wad_size(void)
{
	static uint32_t cached;
	if (cached == 0) cached = *(volatile uint32_t *)MMIO_WAD_SIZE;
	return cached;
}

// DOOMGENERIC_RESX/RESY are NOT defined here on purpose -- a #define in
// this header only takes effect in .c files that #include it, but
// doomgeneric.c (which allocates DG_ScreenBuffer) and i_video.c (which
// writes into it) don't. A mismatch there means every other translation
// unit sees doomgeneric.h's own default (640x400) while this one saw
// 320x200, producing a real stride/size mismatch -- DG_ScreenBuffer gets
// allocated 640x400 but DG_DrawFrame only copies out a 320x200 chunk of
// it, corrupting the image. Must be passed as a global -D compiler flag
// (see the Makefile) so every file agrees.

// Fixed IWAD name handed to doomgeneric_Create's argv, and the one name
// _open() in libc_shim.c will actually serve (see its comment for why).
#define IWAD_NAME "doom1.wad"
