#include "extensions.hpp"
#include <set>

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
	ExtAia = march.find("smaia") != std::string::npos || march.find("ssaia") != std::string::npos;

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
	// "zfh" also matches inside "zfhmin", so full Zfh has to be recognised
	// by its absence: the bare name enables the arithmetic, the "min" form
	// does not. Zfh implies Zfhmin either way.
	{
		size_t p = march.find("zfh");
		if (p != std::string::npos && march.compare(p, 6, "zfhmin") != 0)
			Extensions.ZFH = true;
	}
	// The vector unit's extensions and the deeper page tables, by exact name:
	// several of them are prefixes of others (zvfh of zvfhmin, zvknh of
	// zvknha), which the substring tests above cannot tell apart. The
	// umbrella names expand as the specification defines them.
	{
		std::set<std::string> tokens;
		for (size_t p = march.find('_'); p != std::string::npos;) {
			const size_t q = march.find('_', p + 1);
			tokens.insert(march.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1));
			p = q;
		}
		const auto has = [&](const char *t) { return tokens.count(t) != 0; };
		ExtensionSwitches &e = ExtSwitch;
		const bool zvkn = has("zvkn") || has("zvknc") || has("zvkng");
		const bool zvks = has("zvks") || has("zvksc") || has("zvksg");
		e.ZVFH = has("zvfh");
		e.ZVFHMIN = has("zvfhmin") || e.ZVFH;
		e.ZACAS = has("zacas");
		e.ZABHA = has("zabha");
		e.ZVFBFWMA = has("zvfbfwma");
		e.ZVFBFMIN = has("zvfbfmin") || e.ZVFBFWMA;
		e.ZVBB = has("zvbb");
		e.ZVKB = has("zvkb") || e.ZVBB || zvkn || zvks;
		e.ZVBC = has("zvbc") || has("zvknc") || has("zvksc");
		e.ZVKG = has("zvkg") || has("zvkng") || has("zvksg");
		e.ZVKNED = has("zvkned") || zvkn;
		e.ZVKNHB = has("zvknhb") || zvkn;
		e.ZVKNHA = has("zvknha") || e.ZVKNHB;
		e.ZVKSED = has("zvksed") || zvks;
		e.ZVKSH = has("zvksh") || zvks;
		e.SV57 = has("sv57");
		e.SV48 = has("sv48") || e.SV57;
		e.SVADU = has("svadu");
	}
	if (march.find("svinval") != std::string::npos) Extensions.SVINVAL = true;
	if (march.find("svnapot") != std::string::npos) Extensions.SVNAPOT = true;
	if (march.find("svpbmt") != std::string::npos) Extensions.SVPBMT = true;
	if (march.find("sscofpmf") != std::string::npos) Extensions.SSCOFPMF = true;
	if (march.find("ssstateen") != std::string::npos) Extensions.SSSTATEEN = true;
	if (march.find("ssnpm") != std::string::npos || march.find("smnpm") != std::string::npos
	    || march.find("sspm") != std::string::npos) Extensions.SSNPM = true;
	// Zkn and Zks are umbrella names that imply the bitmanip pieces: Zkn
	// pulls in Zbkb and Zbkx, Zks pulls in Zbkb, Zbkx and Zbc. Naming a
	// piece directly works too.
	if (march.find("zbc") != std::string::npos) Extensions.ZBC = true;
	if (march.find("zbkb") != std::string::npos) Extensions.ZBKB = true;
	if (march.find("zbkx") != std::string::npos) Extensions.ZBKX = true;
	if (march.find("zkn") != std::string::npos)
		Extensions.ZBKB = Extensions.ZBKX = true;
	if (march.find("zks") != std::string::npos)
		Extensions.ZBKB = Extensions.ZBKX = Extensions.ZBC = true;
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

std::string march_with_switches(const std::string &march, const ExtensionSwitches &s)
{
	static const std::set<std::string> names = {
		"zvfh", "zvfhmin", "zvfbfmin", "zvfbfwma", "zvbb", "zvkb", "zvbc", "zvkg", "zvkned", "zvknha", "zvknhb",
		"zvksed", "zvksh", "zvkn", "zvknc", "zvkng", "zvks", "zvksc", "zvksg", "sv48", "sv57", "svadu", "zacas", "zabha"};
	std::string out;
	size_t start = 0;
	for (;;) {
		const size_t end = march.find('_', start);
		const std::string tok = march.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (out.empty()) out = tok;
		else if (!names.count(tok)) out += "_" + tok;
		if (end == std::string::npos) break;
		start = end + 1;
	}
	const std::pair<bool, const char *> on[] = {
		{s.ZVFH, "zvfh"}, {s.ZVFHMIN, "zvfhmin"}, {s.ZVFBFMIN, "zvfbfmin"}, {s.ZVFBFWMA, "zvfbfwma"},
		{s.ZVBB, "zvbb"}, {s.ZVKB, "zvkb"}, {s.ZVBC, "zvbc"}, {s.ZVKG, "zvkg"}, {s.ZVKNED, "zvkned"},
		{s.ZVKNHA, "zvknha"}, {s.ZVKNHB, "zvknhb"}, {s.ZVKSED, "zvksed"}, {s.ZVKSH, "zvksh"},
		{s.SV48, "sv48"}, {s.SV57, "sv57"}, {s.SVADU, "svadu"}, {s.ZACAS, "zacas"}, {s.ZABHA, "zabha"}};
	for (const auto &f : on)
		if (f.first) out += std::string("_") + f.second;
	return out;
}
