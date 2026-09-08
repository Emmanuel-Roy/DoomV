#include "extensions.hpp"

void parse_march(const std::string &march)
{
	Extensions.I = true; // base integer ISA is implied by any march string
	Extensions.M = Extensions.A = Extensions.C = Extensions.F = Extensions.D
	             = Extensions.ZICSR = Extensions.ZIFENCEI = Extensions.V = false;
	Extensions.ZBA = Extensions.ZBB = Extensions.ZBS = false;
	Extensions.ZICOND = false;
	Extensions.ZIHINTPAUSE = Extensions.ZIHINTNTL = Extensions.ZIMOP = Extensions.ZCMOP = false;
	Extensions.ZICBOM = Extensions.ZICBOP = false;
	Extensions.ZICBOZ = Extensions.ZAWRS = Extensions.ZFA = false;
	Extensions.ZFHMIN = false;
	Extensions.SVINVAL = Extensions.SVNAPOT = Extensions.SVPBMT = false;
	Extensions.SSNPM = false;
	Extensions.H = false;
	Extensions.SSCOFPMF = Extensions.SSSTATEEN = false;

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
		case 'v': Extensions.V = true; break;
		case 'h': Extensions.H = true; break;
		case 'g': Extensions.I = Extensions.M = Extensions.A = Extensions.F = Extensions.D = true; break;
		default: break; // unrecognized letter -- ignored, not a strict validator
		}
	}

	if (march.find("zicsr") != std::string::npos) Extensions.ZICSR = true;
	if (march.find("zifencei") != std::string::npos) Extensions.ZIFENCEI = true;
	if (march.find("zba") != std::string::npos) Extensions.ZBA = true;
	if (march.find("zbb") != std::string::npos) Extensions.ZBB = true;
	if (march.find("zbs") != std::string::npos) Extensions.ZBS = true;
	if (march.find("zicond") != std::string::npos) Extensions.ZICOND = true;
	if (march.find("zihintpause") != std::string::npos) Extensions.ZIHINTPAUSE = true;
	if (march.find("zihintntl") != std::string::npos) Extensions.ZIHINTNTL = true;
	if (march.find("zimop") != std::string::npos) Extensions.ZIMOP = true;
	if (march.find("zcmop") != std::string::npos) Extensions.ZCMOP = true;
	if (march.find("zicbom") != std::string::npos) Extensions.ZICBOM = true;
	if (march.find("zicbop") != std::string::npos) Extensions.ZICBOP = true;
	if (march.find("zicboz") != std::string::npos) Extensions.ZICBOZ = true;
	if (march.find("zawrs") != std::string::npos) Extensions.ZAWRS = true;
	if (march.find("zfa") != std::string::npos) Extensions.ZFA = true;
	// "zfh" also matches inside "zfhmin"; both imply the minimal set, and
	// full Zfh arithmetic is not implemented (see extensions.hpp).
	if (march.find("zfh") != std::string::npos) Extensions.ZFHMIN = true;
	if (march.find("svinval") != std::string::npos) Extensions.SVINVAL = true;
	if (march.find("svnapot") != std::string::npos) Extensions.SVNAPOT = true;
	if (march.find("svpbmt") != std::string::npos) Extensions.SVPBMT = true;
	if (march.find("sscofpmf") != std::string::npos) Extensions.SSCOFPMF = true;
	if (march.find("ssstateen") != std::string::npos) Extensions.SSSTATEEN = true;
	if (march.find("ssnpm") != std::string::npos || march.find("smnpm") != std::string::npos
	    || march.find("sspm") != std::string::npos) Extensions.SSNPM = true;
	if (march.find("zkr") != std::string::npos) Extensions.ZKR = true;
	if (march.find("zicfilp") != std::string::npos) Extensions.ZICFILP = true;
	if (march.find("zicfiss") != std::string::npos) Extensions.ZICFISS = true;
	// "h" as a single letter, the way misa spells it. Checked against the
	// base-letter loop below rather than a token search, since "h" appears
	// inside plenty of multi-letter names ("zfh", "zihintpause").
	// "b" is the umbrella name for Zba+Zbb+Zbs (the ratified B extension).
	for (char c : base) if (c == 'b') { Extensions.ZBA = Extensions.ZBB = Extensions.ZBS = true; }

	// D without F is a spec violation (D always implies F) -- rather than
	// silently misbehave on the FCVT.S.D/FCVT.D.S instructions that need
	// both, just pull F along with D here.
	// Zcmop is defined as depending on Zimop -- the compressed encodings are
	// the same reserved-but-defined idea in 16 bits, and no toolchain emits
	// one space without the other.
	if (Extensions.ZCMOP) Extensions.ZIMOP = true;

	if (Extensions.ZFA) Extensions.D = true; // fli.d/fminm.d/fcvtmod.w.d need double
	if (Extensions.ZFHMIN) Extensions.F = true; // half converts to and from single

	if (Extensions.D) Extensions.F = true;

	// The V extension (as opposed to one of the embedded Zve* subsets)
	// requires double-precision vector element support, i.e. D -- and
	// transitively F. Vector FP ops reuse the same host-float helpers F/D
	// already built, so this isn't just a spec formality here.
	if (Extensions.V) { Extensions.D = true; Extensions.F = true; }
}
