// Eight breadboard buttons -> DOOM key events.
#pragma once
#include <stdint.h>

void buttons_init(void);
// Sample the buttons (every 5 ms at most; call often) and hand each change,
// as DOOM key events, to `emit`. Runs on core 1.
void buttons_poll(void (*emit)(int pressed, unsigned char key));
// Raw state, bit per button (for the boot-time benchmark combo).
uint32_t buttons_raw(void);

enum { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_FIRE, BTN_USE, BTN_MENU, BTN_WEAPON, BTN_COUNT };
