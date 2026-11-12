// Eight breadboard buttons -> DOOM key events.
#pragma once
#include <stdint.h>

void buttons_init(void);
// Next key event, as DG_GetKey wants it. Returns 0 when there is none.
int buttons_get_key(int *pressed, unsigned char *key);
// Raw state, bit per button (for the boot-time benchmark combo).
uint32_t buttons_raw(void);

enum { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_FIRE, BTN_USE, BTN_MENU, BTN_WEAPON, BTN_COUNT };
