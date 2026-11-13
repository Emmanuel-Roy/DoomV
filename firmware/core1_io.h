// Core 1: all of the Pico's I/O -- display, buttons, USB -- so core 0 only
// runs DOOM. See core1_io.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum io_source { IO_FROM_BUTTONS = 0, IO_FROM_PC = 1 };

// Core 0 calls these.
void io_start(void);                                   // launch core 1
int io_get_key(int *pressed, unsigned char *key, int *source);  // pop the input buffer
// Rows above the last `tail_rows` are read from `top` before this returns;
// the tail (a copy) and pal565 are core 1's until io_present_wait().
void io_present(const uint8_t *top, const uint8_t *tail, int tail_rows, const uint16_t *pal565);
void io_present_wait(void);                            // until core 1 is done reading
void io_hud(const char *text);                         // shown under the next frame
void io_message(const char *title, const char *msg);   // full-screen text, blocking
int io_video_every(void);                              // PC asked for every Nth frame
uint32_t io_lcd_frame_us(void);                        // core 1's time to send the last frame
