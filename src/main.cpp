#include "doom_system.hpp"
#include "extensions.hpp"
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char *argv[])
{
	std::vector<std::string> positional;
	std::string march;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg.rfind("-march=", 0) == 0) {
			march = arg.substr(7);
		} else {
			positional.push_back(arg);
		}
	}

	// No -march= given -> Extensions keeps ExtensionConfig's defaults
	// (rv64imafdc_zicsr-equivalent). parse_march() resets everything, so
	// only call it when the flag was actually passed.
	if (!march.empty()) parse_march(march);

	if (positional.size() < 2) {
		std::cout << "Usage: " << argv[0] << " <wad_path> <elf_path> [-march=rv64imafdc_zicsr]\n";
		return -1;
	}

	DoomSystem system;
	if (!system.init(positional[0].c_str(), positional[1].c_str())) {
		return -1;
	}

	system.run();
	return 0;
}
