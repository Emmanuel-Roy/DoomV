#pragma once

// Guest-side memory map. Must match the host's Memory bus dispatch.
//
// Reading MMIO_INPUT is destructive (pops one queued key event, 0 if
// none pending) -- the event queue itself lives host-side in Memory,
// populated each frame from Gui's input polling. Layout: bits 15-8 are
// pressed(1)/released(0), bits 7-0 are the Doom key code. The SDL
// keysym -> Doom key code translation also happens host-side, so this
// file only ever sees already-translated key codes.
#define MMIO_INPUT   0x10000000u

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

// RAM (.text/.data/.bss/heap/stack) starts right after the framebuffer
// page -- see riscv.lds. 16MB region, WAD sits right after it ends.
#define RAM_BASE     0x10041000u
#define RAM_SIZE     0x01000000u   // 16MB

// WAD blob, placed by the host loader, read directly by w_file_doomv.c.
// 20MB covers the largest real WAD (Final Doom's plutonia.wad/tnt.wad,
// ~18-19MB) with headroom, well above doom2.wad (~14MB) or doom.wad (~12MB).
#define WAD_BASE     0x11041000u
#define WAD_SIZE     0x01400000u   // 20MB

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
