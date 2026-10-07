#include "doom_system.hpp"
#include "machine.hpp"
#include <memory>

int main(int argc, char *argv[])
{
	std::unique_ptr<DoomSystem> system;
	int status = 0;
	if (!setup_machine(argc, argv, system, status)) return status;
	system->run();
	return 0;
}
