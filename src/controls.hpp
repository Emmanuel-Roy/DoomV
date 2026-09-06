#pragma once
#include <cstdint>
#include <unordered_map>

// Loads controls.json's movement bindings (forward/backward/turn_left/
// turn_right -> a key name). Everything else (fire, use, menu keys) is
// fixed in DoomSystem::translate_key, not remappable here.
class ControlMap {
public:
	ControlMap();

	void load(const char *json_path);

	// Returns 0 if sdl_keysym isn't one of the bound movement keys.
	uint8_t translate(uint32_t sdl_keysym) const;

private:
	std::unordered_map<uint32_t, uint8_t> bindings;
};
