#include "extensions.hpp"

void parse_march(const std::string &march)
{
	Extensions.I = true; // base integer ISA is implied by any march string
	Extensions.M = Extensions.A = Extensions.C = Extensions.F = Extensions.D = Extensions.ZICSR = false;

	size_t pos = 0;
	if (march.rfind("rv64", 0) == 0) { Extensions.XLEN64 = true; pos = 4; }
	else if (march.rfind("rv32", 0) == 0) { Extensions.XLEN64 = false; pos = 4; }

	size_t underscore = march.find('_', pos);
	std::string base = march.substr(pos, underscore == std::string::npos ? std::string::npos : underscore - pos);

	for (char c : base) {
		switch (c) {
		case 'i': Extensions.I = true; break;
		case 'm': Extensions.M = true; break;
		case 'a': Extensions.A = true; break;
		case 'f': Extensions.F = true; break;
		case 'd': Extensions.D = true; break;
		case 'c': Extensions.C = true; break;
		case 'g': Extensions.I = Extensions.M = Extensions.A = Extensions.F = Extensions.D = true; break;
		default: break; // unrecognized letter -- ignored, not a strict validator
		}
	}

	if (march.find("zicsr") != std::string::npos) Extensions.ZICSR = true;

	// D without F is a spec violation (D always implies F) -- rather than
	// silently misbehave on the FCVT.S.D/FCVT.D.S instructions that need
	// both, just pull F along with D here.
	if (Extensions.D) Extensions.F = true;
}
