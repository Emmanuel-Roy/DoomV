#include "doom_system.hpp"
#include <iostream>

int main(int argc, char *argv[])
{
	if (argc < 3) {
		std::cout << "Usage: " << argv[0] << " <wad_path> <elf_path>\n";
		return -1;
	}

	DoomSystem system;
	if (!system.init(argv[1], argv[2])) {
		return -1;
	}

	system.run();
	return 0;
}
