#include "doom_system.hpp"
#include <iostream>

int main(int argc, char *argv[])
{
	if (argc < 3) {
		std::cout << "Usage: " << argv[0] << " <wad_path> <elf_path>\n";
		return -1;
	}

	int choice = 1;
	std::cout << "Select Resolution:\n1. 360p (Original)\n2. 720p\n3. 1080p\nChoice: ";
	std::cin >> choice;
	if (choice < 1 || choice > 3) choice = 1;

	DoomSystem system;
	if (!system.init(argv[1], argv[2], choice)) {
		return -1;
	}

	system.run();
	return 0;
}
