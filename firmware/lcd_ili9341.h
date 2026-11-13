// ILI9341 320x240 SPI display.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LCD_W 320
#define LCD_H 240
#define LCD_GAME_Y 0              // DOOM's 320x200 picture: rows 0-199
#define LCD_HUD_Y 200             // perf read-out: rows 200-239
#define LCD_HUD_H (LCD_H - LCD_HUD_Y)

// Returns the SPI clock actually achieved, in Hz.
uint32_t lcd_init(uint32_t spi_hz);

// Send a 320x200 8-bit frame, converting through the RGB565 palette as it
// goes. Blocks until the last pixel is in the SPI FIFO.
void lcd_present_frame(const uint8_t *frame, const uint16_t *pal565);

// The same, as a job advanced in steps (core 1 runs it between other work):
// begin, then call step until it returns true. Rows 0 .. 199-tail_rows come
// from `top`, the last `tail_rows` from `tail` (a copy). lcd_rows_read()
// says how many rows have been read: `top` is free once it passes the split.
void lcd_frame_begin(const uint8_t *top, const uint8_t *tail, int tail_rows, const uint16_t *pal565);
bool lcd_frame_step(void);
int lcd_rows_read(void);

// Draw up to two lines of text in the 40 rows under the picture.
void lcd_hud_text(const char *line1, const char *line2);

// Fill the whole screen with one colour, then print a message (for errors).
void lcd_message(const char *title, const char *msg);
