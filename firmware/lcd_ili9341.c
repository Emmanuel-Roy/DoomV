// ILI9341 over SPI, fed by DMA from two small strip buffers.
//
// DOOM draws an 8-bit (palette-indexed) frame. The panel wants RGB565, but a
// full RGB565 copy would be 128 KB -- a quarter of the chip -- so the frame
// is converted 2 lines at a time: the CPU fills one 2-line strip while DMA
// streams the other to the SPI FIFO. Total buffer cost: 2.5 KB. (Converting
// 640 pixels takes ~15 us; sending them, ~270 us -- the CPU is never the
// bottleneck, so bigger strips would buy nothing.)
//
// The SPI runs in 16-bit mode during pixel data, so each RGB565 value goes
// out most-significant byte first -- the order the ILI9341 wants -- with no
// byte swapping.

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"

#include "board_config.h"
#include "lcd_ili9341.h"

#define STRIP_LINES 2
static uint16_t strip[2][STRIP_LINES * LCD_W];
static int dma_ch = -1;

static inline void cs(bool on) { gpio_put(LCD_PIN_CS, !on); }
static inline void dc_data(bool data) { gpio_put(LCD_PIN_DC, data); }

static void wait_spi_idle(void)
{
	while (spi_is_busy(LCD_SPI)) tight_loop_contents();
}

static void bits(int n)
{
	wait_spi_idle();
	spi_set_format(LCD_SPI, n, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

static void cmd(uint8_t c, const uint8_t *data, int n)
{
	bits(8);
	cs(true);
	dc_data(false);
	spi_write_blocking(LCD_SPI, &c, 1);
	if (n) {
		dc_data(true);
		spi_write_blocking(LCD_SPI, data, n);
	}
	wait_spi_idle();
	cs(false);
}

#define CMD(c, ...) do { static const uint8_t d_[] = { __VA_ARGS__ }; cmd(c, d_, sizeof d_); } while (0)

// Leave CS low and the panel in "memory write" with 16-bit SPI frames.
static void begin_window(int x0, int y0, int x1, int y1)
{
	uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
	uint8_t pa[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
	cmd(0x2A, ca, 4);   // column address set
	cmd(0x2B, pa, 4);   // page address set
	bits(8);
	cs(true);
	dc_data(false);
	uint8_t ramwr = 0x2C;
	spi_write_blocking(LCD_SPI, &ramwr, 1);
	wait_spi_idle();
	dc_data(true);
	bits(16);
}

static void end_window(void)
{
	if (dma_ch >= 0) dma_channel_wait_for_finish_blocking(dma_ch);
	wait_spi_idle();
	cs(false);
}

static void send_strip(const uint16_t *px, int count)
{
	dma_channel_wait_for_finish_blocking(dma_ch);
	dma_channel_set_read_addr(dma_ch, px, false);
	dma_channel_set_trans_count(dma_ch, count, true);
}

uint32_t lcd_init(uint32_t spi_hz)
{
	uint32_t actual = spi_init(LCD_SPI, spi_hz);
	gpio_set_function(LCD_PIN_SCK, GPIO_FUNC_SPI);
	gpio_set_function(LCD_PIN_MOSI, GPIO_FUNC_SPI);
	// Short breadboard wires at tens of MHz: a strong, fast edge helps.
	gpio_set_drive_strength(LCD_PIN_SCK, GPIO_DRIVE_STRENGTH_8MA);
	gpio_set_drive_strength(LCD_PIN_MOSI, GPIO_DRIVE_STRENGTH_8MA);
	gpio_set_slew_rate(LCD_PIN_SCK, GPIO_SLEW_RATE_FAST);
	gpio_set_slew_rate(LCD_PIN_MOSI, GPIO_SLEW_RATE_FAST);

	const uint pins[] = { LCD_PIN_CS, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL };
	for (unsigned i = 0; i < 4; i++) {
		gpio_init(pins[i]);
		gpio_set_dir(pins[i], GPIO_OUT);
		gpio_put(pins[i], 1);
	}

	gpio_put(LCD_PIN_RST, 0);
	sleep_ms(10);
	gpio_put(LCD_PIN_RST, 1);
	sleep_ms(120);

	cmd(0x01, NULL, 0);                      // software reset
	sleep_ms(150);
	CMD(0xC0, 0x23);                         // power control 1
	CMD(0xC1, 0x10);                         // power control 2
	CMD(0xC5, 0x3E, 0x28);                   // VCOM control 1
	CMD(0xC7, 0x86);                         // VCOM control 2
	CMD(0x36, LCD_MADCTL);                   // memory access: orientation
	CMD(0x37, 0x00);                         // vertical scroll start
	CMD(0x3A, 0x55);                         // 16 bits per pixel
	CMD(0xB1, 0x00, 0x18);                   // frame rate 79 Hz
	CMD(0xB6, 0x08, 0x82, 0x27);             // display function control
	CMD(0xF2, 0x00);                         // 3-gamma off
	CMD(0x26, 0x01);                         // gamma curve 1
	CMD(0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);
	CMD(0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);
	cmd(0x11, NULL, 0);                      // sleep out
	sleep_ms(120);
	cmd(0x29, NULL, 0);                      // display on

	dma_ch = dma_claim_unused_channel(true);
	dma_channel_config c = dma_channel_get_default_config(dma_ch);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	dma_channel_configure(dma_ch, &c, &spi_get_hw(LCD_SPI)->dr, strip[0], 0, false);

	lcd_message("Pico2Doom", "Starting DOOM...");
	return actual;
}

void lcd_present_frame(const uint8_t *frame, const uint16_t *pal565)
{
	begin_window(0, LCD_GAME_Y, LCD_W - 1, LCD_GAME_Y + 199);
	for (int y = 0, b = 0; y < 200; y += STRIP_LINES, b ^= 1) {
		// strip[b] was last sent two strips ago; send_strip waited for it.
		uint16_t *out = strip[b];
		const uint8_t *in = frame + y * LCD_W;
		for (int i = 0; i < STRIP_LINES * LCD_W; i += 4) {
			out[i + 0] = pal565[in[i + 0]];
			out[i + 1] = pal565[in[i + 1]];
			out[i + 2] = pal565[in[i + 2]];
			out[i + 3] = pal565[in[i + 3]];
		}
		send_strip(out, STRIP_LINES * LCD_W);
	}
	end_window();
}

// ---- text -----------------------------------------------------------------

extern const uint8_t hud_font5x7[95][5];

// Render pixel row `row` (0-9) of one 53-character text line into `out`.
static void render_text_row(uint16_t *out, int row, const char *text, int len, uint16_t fg, uint16_t bg)
{
	for (int i = 0; i < LCD_W; i++) out[i] = bg;
	int glyph_row = row - 1;              // one blank pixel row above each line
	if (glyph_row < 0 || glyph_row > 7) return;
	for (int c = 0; c < len && c < LCD_W / 6; c++) {
		int ch = (unsigned char)text[c];
		if (ch < 32 || ch > 126) ch = '?';
		const uint8_t *g = hud_font5x7[ch - 32];
		for (int col = 0; col < 5; col++)   // glyph row 7: descenders (g, p, y)
			if (g[col] & (1 << glyph_row)) out[c * 6 + col + 1] = fg;
	}
}

// Draw wrapped text into rows [y0, y0 + nlines*10), one pixel row at a time
// through the two strip buffers (text is drawn rarely; RAM is not spare).
static void draw_text_block(int y0, int nlines, const char *text, uint16_t fg, uint16_t bg)
{
	const int per_line = LCD_W / 6;
	int len = (int)strlen(text);
	begin_window(0, y0, LCD_W - 1, y0 + nlines * 10 - 1);
	for (int l = 0, b = 0; l < nlines; l++) {
		const char *s = text + l * per_line;
		int n = len - l * per_line;
		if (n < 0) n = 0;
		if (n > per_line) n = per_line;
		for (int row = 0; row < 10; row++, b ^= 1) {
			render_text_row(strip[b], row, s, n, fg, bg);
			send_strip(strip[b], LCD_W);
		}
	}
	end_window();
}

void lcd_hud_text(const char *line1, const char *line2)
{
	static char buf[256];
	// Two logical lines, each wrapped onto up to two 53-character rows.
	const int per_line = LCD_W / 6;
	memset(buf, ' ', sizeof buf);
	int n1 = (int)strlen(line1), n2 = line2 ? (int)strlen(line2) : 0;
	if (n1 > 2 * per_line) n1 = 2 * per_line;
	if (n2 > 2 * per_line) n2 = 2 * per_line;
	memcpy(buf, line1, n1);
	if (n2) memcpy(buf + 2 * per_line, line2, n2);
	buf[4 * per_line] = 0;
	draw_text_block(LCD_HUD_Y, 4, buf, 0xFFE0 /* yellow */, 0x0000);
}

void lcd_message(const char *title, const char *msg)
{
	const int per_line = LCD_W / 6;
	static char buf[24 * 53 + 1];
	memset(buf, ' ', sizeof buf - 1);
	buf[sizeof buf - 1] = 0;
	int tl = (int)strlen(title);
	if (tl > per_line) tl = per_line;
	memcpy(buf, title, tl);
	// Message from row 2, honouring '\n'.
	int row = 2, col = 0;
	for (const char *p = msg; *p && row < 24; p++) {
		if (*p == '\n' || col == per_line) { row++; col = 0; if (*p == '\n') continue; }
		if (row < 24) buf[row * per_line + col++] = *p;
	}
	draw_text_block(0, 24, buf, 0xFFFF, 0x8000 /* dark red */);
}
