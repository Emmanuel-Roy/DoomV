#pragma once
#include <string>

// Runtime-configurable, not compile-time -- every extension (and the base
// XLEN width) can be switched on/off per run via -march= on the command
// line (see parse_march below and main.cpp), no rebuild required. This
// used to be a namespace of `constexpr bool`s; the decoder/core still read
// these the same way at every call site (just `Extensions.I` instead of
// `Extensions::I` now), so making a wrong runtime choice is exactly as
// safe as it was at compile time -- classify()/decode() already treat any
// disabled extension's instructions as illegal, gated fresh on every call.
struct ExtensionConfig {
	bool I = true;
	bool M = true;
	bool A = true;
	bool C = true;
	bool ZICSR = true;
	bool ZIFENCEI = true;
	// D requires F per spec (a hart can't have double without single) --
	// parse_march enforces this; hand-setting the fields yourself doesn't.
	bool F = true;
	bool D = true;
	bool V = false; // off by default like the rest -- opt in with -march=...v

	// Base ISA width, not an optional extension. Registers/Memory always
	// store values in 64-bit containers regardless of this flag: RV32
	// mode just means every integer op computes at 32-bit width and
	// sign-extends its result into that container (the same mechanism
	// RV64's *W-suffixed instructions use), rather than every register
	// genuinely being a narrower type. That's what keeps Registers/
	// Memory/Snapshot/Gui untouched by this flag -- only the decoder
	// (RV64-only opcodes, RV32-vs-RV64 compressed-encoding meanings) and
	// RiscvCore's compute width need to branch on it.
	bool XLEN64 = true;
};

// Single global instance, mutated once at startup by parse_march() (or left
// at its rv64imafdc_zicsr_zifencei-equivalent defaults above) before
// DoomSystem constructs anything that reads it. Not touched again after
// that -- reads happen constantly (every decode), writes happen at most
// once per run.
inline ExtensionConfig Extensions;

// Parses a GCC/toolchain-style march string ("rv64imafdc_zicsr",
// "rv32ima", ...): resets every extension to off, sets XLEN64 from the
// rv32/rv64 prefix, turns on one flag per recognized base letter (i/m/a/f/
// d/c, plus g as shorthand for imafd), and checks for "zicsr"/"zifencei"
// tokens after an underscore. Unrecognized letters/tokens are silently
// ignored (this isn't trying to be a strict validator, just a convenience
// toggle).
void parse_march(const std::string &march);
