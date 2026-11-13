// DoomV's devices, as the guest sees them. The first block is DoomV's existing
// DOOM interface (its src/memory.hpp); the link registers are the Pico2Doom
// lock-step device added to DoomV (its src/pico_link.cpp).
#pragma once

#define MMIO_INPUT      0x10000000u  // R: next key event (pressed<<8 | key), 0 = none
#define MMIO_TICK       0x10000004u  // R: milliseconds (instruction-count based)
#define MMIO_DEBUG      0x10000008u  // W: one byte to DoomV's console
#define MMIO_WAD_BASE   0x10000014u  // R: guest address of the WAD
#define MMIO_WAD_SIZE   0x10000018u  // R: its length
#define MMIO_FB         0x10200000u  // 320x200 words, 0x00RRGGBB, shown in DoomV's window

// Pico link: DoomV parses the Pico's recording (-picolink=FILE) and serves it.
#define MMIO_LINK_ZONE  0x1000001Cu  // R: the Pico's zone size; 0 = no recording attached
#define MMIO_LINK_TICKS 0x10000020u  // R: the next clock value the Pico read
#define MMIO_LINK_KEY   0x10000024u  // R: the next DG_GetKey result: 0, or 0x10000|pressed<<8|key
#define MMIO_LINK_FRAME 0x10000028u  // W x5: frame, frame hash, state hash,
                                     //       retired instructions (logic, render);
                                     //       DoomV compares on the fifth word
#define MMIO_LINK_ARGS  0x1000002Cu  // R: next byte of the Pico's DOOM arguments, 0 at the end
