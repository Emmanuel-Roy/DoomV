// Wiring. Change these to match your breadboard; README.md has the diagram.
#pragma once

// ---- ILI9341 display on SPI0 ---------------------------------------------
#define LCD_SPI        spi0
#define LCD_PIN_SCK    18   // SCK / CLK
#define LCD_PIN_MOSI   19   // SDI / MOSI
#define LCD_PIN_CS     17   // CS
#define LCD_PIN_DC     20   // DC / RS
#define LCD_PIN_RST    21   // RESET
#define LCD_PIN_BL     22   // LED / backlight (or tie it to 3V3 and ignore)

// Landscape, 320x240 (Adafruit's setRotation(1)). If your picture is
// upside down try 0xE8; mirrored, 0x68 or 0xA8; red and blue swapped, flip
// bit 0x08 (BGR).
#define LCD_MADCTL     0x28

// ---- buttons: each between the GPIO and GND (internal pull-ups) ----------
#define BTN_PIN_UP      2
#define BTN_PIN_DOWN    3
#define BTN_PIN_LEFT    4
#define BTN_PIN_RIGHT   5
#define BTN_PIN_FIRE    6   // also "select" in menus
#define BTN_PIN_USE     7   // open doors, flip switches; also "yes"
#define BTN_PIN_MENU    8   // Esc: open/close the menu
#define BTN_PIN_WEAPON  9   // tap: next weapon. Hold + LEFT/RIGHT: strafe.
                            // Hold + UP: automap.
