#include "Doom_System.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    int choice = 1;
    std::cout << "Select Resolution:\n1. 360p (Original)\n2. 720p\n3. 1080p\nChoice: ";
    std::cin >> choice;

    if (choice < 1 || choice > 3) choice = 1;

    Doom_System system;
    if (!system.init_sdl(choice)) {
        return -1;
    }

    system.run();
    return 0;
}