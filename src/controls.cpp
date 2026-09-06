#include "controls.hpp"
#include <SDL2/SDL.h>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// Doom key codes for the 4 movement actions controls.json can bind (see
// doomkeys.h: KEY_UPARROW/DOWNARROW/LEFTARROW/RIGHTARROW).
const std::unordered_map<std::string, uint8_t> ACTION_TO_DOOMKEY = {
	{"forward",    0xad},
	{"backward",   0xaf},
	{"turn_left",  0xac},
	{"turn_right", 0xae},
};

const std::unordered_map<std::string, uint32_t> KEYNAME_TO_SDLKEY = {
	{"W", SDLK_w}, {"A", SDLK_a}, {"S", SDLK_s}, {"D", SDLK_d},
	{"UP", SDLK_UP}, {"DOWN", SDLK_DOWN}, {"LEFT", SDLK_LEFT}, {"RIGHT", SDLK_RIGHT},
};

// Minimal parser for controls.json's actual shape only: a flat
// {"action": "KEY", ...} object of strings -- no nesting, arrays,
// numbers, or escape sequences needed.
std::unordered_map<std::string, std::string> parse_flat_json(const std::string &text)
{
	std::unordered_map<std::string, std::string> result;
	size_t i = 0;

	auto skip_ws = [&]() { while (i < text.size() && std::isspace((unsigned char)text[i])) i++; };
	auto parse_string = [&]() -> std::string {
		skip_ws();
		if (i >= text.size() || text[i] != '"') return "";
		i++;
		std::string s;
		while (i < text.size() && text[i] != '"') s += text[i++];
		if (i < text.size()) i++; // closing quote
		return s;
	};

	skip_ws();
	if (i >= text.size() || text[i] != '{') return result;
	i++;

	while (true) {
		skip_ws();
		if (i >= text.size() || text[i] == '}') break;

		std::string key = parse_string();
		skip_ws();
		if (i < text.size() && text[i] == ':') i++;
		std::string value = parse_string();
		result[key] = value;

		skip_ws();
		if (i < text.size() && text[i] == ',') { i++; continue; }
		break;
	}

	return result;
}

} // namespace

ControlMap::ControlMap()
{
}

void ControlMap::load(const char *json_path)
{
	std::ifstream file(json_path);
	if (!file.is_open()) return;

	std::stringstream ss;
	ss << file.rdbuf();
	auto entries = parse_flat_json(ss.str());

	for (const auto &entry : entries) {
		auto action_it = ACTION_TO_DOOMKEY.find(entry.first);
		auto key_it = KEYNAME_TO_SDLKEY.find(entry.second);
		if (action_it != ACTION_TO_DOOMKEY.end() && key_it != KEYNAME_TO_SDLKEY.end()) {
			bindings[key_it->second] = action_it->second;
		}
	}
}

uint8_t ControlMap::translate(uint32_t sdl_keysym) const
{
	auto it = bindings.find(sdl_keysym);
	return (it != bindings.end()) ? it->second : 0;
}
