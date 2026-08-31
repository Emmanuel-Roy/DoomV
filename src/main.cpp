#include "doom_system.hpp"
#include "extensions.hpp"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char *argv[])
{
	std::vector<std::string> positional;
	std::string march;
	uint64_t breakpoint = 0;
	bool have_breakpoint = false;
	uint64_t sig_begin = 0, sig_end = 0;
	bool have_sig = false;
	std::string opensbi_path, kernel_path, dtb_path;
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg.rfind("-march=", 0) == 0) {
			march = arg.substr(7);
		} else if (arg.rfind("-break=", 0) == 0) {
			breakpoint = std::stoull(arg.substr(7), nullptr, 16);
			have_breakpoint = true;
		} else if (arg.rfind("-sig=", 0) == 0) {
			std::string range = arg.substr(5);
			size_t colon = range.find(':');
			sig_begin = std::stoull(range.substr(0, colon), nullptr, 16);
			sig_end = std::stoull(range.substr(colon + 1), nullptr, 16);
			have_sig = true;
		} else if (arg.rfind("-opensbi=", 0) == 0) {
			opensbi_path = arg.substr(9);
		} else if (arg.rfind("-kernel=", 0) == 0) {
			kernel_path = arg.substr(8);
		} else if (arg.rfind("-dtb=", 0) == 0) {
			dtb_path = arg.substr(5);
		} else {
			positional.push_back(arg);
		}
	}

	// No -march= given -> Extensions keeps ExtensionConfig's defaults
	// (rv64imafdc_zicsr-equivalent). parse_march() resets everything, so
	// only call it when the flag was actually passed.
	if (!march.empty()) parse_march(march);

	bool linux_boot = !opensbi_path.empty() || !kernel_path.empty() || !dtb_path.empty();
	if (linux_boot && (opensbi_path.empty() || kernel_path.empty() || dtb_path.empty())) {
		std::cout << "Usage: " << argv[0] << " -opensbi=<path> -kernel=<path> -dtb=<path> [-march=...] [-break=<hex_pc>]\n";
		return -1;
	}

	DoomSystem system;
	if (linux_boot) {
		if (!system.init_linux_boot(opensbi_path.c_str(), kernel_path.c_str(), dtb_path.c_str())) {
			return -1;
		}
	} else {
		if (positional.size() < 2) {
			std::cout << "Usage: " << argv[0] << " <wad_path> <elf_path> [-march=rv64imafdc_zicsr] [-break=<hex_pc>] [-sig=<hex_begin>:<hex_end>]\n"
			          << "   or: " << argv[0] << " -opensbi=<path> -kernel=<path> -dtb=<path> [-march=...] [-break=<hex_pc>]\n";
			return -1;
		}
		if (!system.init(positional[0].c_str(), positional[1].c_str())) {
			return -1;
		}
	}
	if (have_breakpoint) system.add_breakpoint(breakpoint);
	if (have_sig) system.set_signature_range(sig_begin, sig_end, "signature.log");

	system.run();
	return 0;
}
