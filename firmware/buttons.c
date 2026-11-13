// Eight buttons to DOOM key events.
//
// DOOM wants key-down / key-up events, so the pins are sampled every 5 ms
// (which also debounces them) by core 1, and every change becomes events
// handed to `emit` -- core1_io.c puts them in the input buffer that
// DG_GetKey drains on core 0. Some buttons send two keys because
// DOOM uses different keys in the menus and in the game:
//
//   FIRE  -> fire  + Enter (select in menus)
//   USE   -> use   + 'y'   (answer "yes" to prompts)
//   MENU  -> Esc
//   WEAPON tapped           -> next weapon
//   WEAPON held + LEFT/RIGHT -> strafe left/right instead of turning
//   WEAPON held + UP         -> automap (Tab)

#include "pico/stdlib.h"

#include "doomkeys.h"
#include "port_keys.h"
#include "board_config.h"
#include "buttons.h"

static const uint8_t pins[BTN_COUNT] = {
	BTN_PIN_UP, BTN_PIN_DOWN, BTN_PIN_LEFT, BTN_PIN_RIGHT,
	BTN_PIN_FIRE, BTN_PIN_USE, BTN_PIN_MENU, BTN_PIN_WEAPON,
};

static uint32_t state;              // debounced, bit per button, 1 = held
static uint64_t last_sample_us;
static uint8_t sent[BTN_COUNT];     // the key each held button sent on press
static int weapon_used_as_modifier;

static void (*emit)(int pressed, unsigned char key);

static void push(int pressed, unsigned char key) { emit(pressed, key); }

void buttons_init(void)
{
	for (int i = 0; i < BTN_COUNT; i++) {
		gpio_init(pins[i]);
		gpio_set_dir(pins[i], GPIO_IN);
		gpio_pull_up(pins[i]);
	}
	sleep_us(10);
	state = buttons_raw();     // don't report buttons already held at boot
}

uint32_t buttons_raw(void)
{
	uint32_t s = 0;
	for (int i = 0; i < BTN_COUNT; i++)
		if (!gpio_get(pins[i])) s |= 1u << i;
	return s;
}

static void press(int b)
{
	int modifier = (state >> BTN_WEAPON) & 1;
	unsigned char k = 0;
	switch (b) {
	case BTN_UP:    k = modifier ? KEY_TAB : KEY_UPARROW; break;
	case BTN_DOWN:  k = KEY_DOWNARROW; break;
	case BTN_LEFT:  k = modifier ? KEY_STRAFE_L : KEY_LEFTARROW; break;
	case BTN_RIGHT: k = modifier ? KEY_STRAFE_R : KEY_RIGHTARROW; break;
	case BTN_FIRE:  k = KEY_FIRE;   push(1, KEY_ENTER); break;
	case BTN_USE:   k = KEY_USE;    push(1, 'y'); break;
	case BTN_MENU:  k = KEY_ESCAPE; break;
	case BTN_WEAPON: weapon_used_as_modifier = 0; return;   // decided on release
	}
	if (modifier && (b == BTN_UP || b == BTN_LEFT || b == BTN_RIGHT)) weapon_used_as_modifier = 1;
	sent[b] = k;
	push(1, k);
}

static void release(int b)
{
	switch (b) {
	case BTN_FIRE: push(0, KEY_ENTER); break;
	case BTN_USE:  push(0, 'y'); break;
	case BTN_WEAPON:
		if (!weapon_used_as_modifier) {      // a tap: next weapon
			push(1, PORT_KEY_NEXTWEAPON);
			push(0, PORT_KEY_NEXTWEAPON);
		}
		return;
	}
	if (sent[b]) push(0, sent[b]);   // release what was pressed, even if WEAPON changed since
	sent[b] = 0;
}

void buttons_poll(void (*emit_fn)(int pressed, unsigned char key))
{
	emit = emit_fn;
	uint64_t now = time_us_64();
	if (now - last_sample_us < 5000) return;
	last_sample_us = now;

	uint32_t raw = buttons_raw();
	uint32_t changed = raw ^ state;
	for (int b = 0; b < BTN_COUNT; b++) {
		if (!(changed & (1u << b))) continue;
		if (raw & (1u << b)) {
			state |= 1u << b;
			press(b);
		} else {
			state &= ~(1u << b);
			release(b);
		}
	}
}

