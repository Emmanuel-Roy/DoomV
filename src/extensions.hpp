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
	// Zba/Zbb/Zbs default ON, unlike V. Bare-metal Doom never emits them,
	// but every modern riscv64 Linux userspace assumes them (the RVA23
	// profile mandates all three), so defaulting them off would just mean
	// every distro binary halts on its first sh*add.
	bool ZBA = true;
	bool ZBB = true;
	bool ZBS = true;
	bool ZICOND = true; // czero.eqz/czero.nez -- RVA23 again, same reason as Zb*


	// The RVA23 hint and reserved-encoding extensions. All six retire
	// without architectural effect on this machine, and several were
	// already retiring correctly by accident -- PAUSE is a FENCE, the NTL
	// hints are C.ADD into x0, and the prefetches are ORI into x0. What
	// enabling them buys is that the machine names them honestly, and that
	// Zimop/Zcmop write the zero the spec requires rather than leaving rd
	// untouched, which is the one place a "does nothing" extension can
	// actually be wrong.
	//
	// Default on for the same reason as Zb*: RVA23 mandates them, so a
	// distro userspace may emit them freely and defaulting them off would
	// only manufacture illegal instructions.
	bool ZIHINTPAUSE = true;
	bool ZIHINTNTL = true;
	bool ZIMOP = true;
	bool ZCMOP = true;
	bool ZICBOM = true;
	bool ZICBOP = true;
	// Zicboz has real behaviour, unlike the group above: cbo.zero is a
	// store. Zawrs retires immediately, which the spec explicitly permits
	// and which is the only implementation that terminates on one hart.
	bool ZICBOZ = true;
	bool ZAWRS = true;
	// Zfa is real FP arithmetic, not a hint: fli materialises constants,
	// fminm/fmaxm differ from FMIN/FMAX on NaN, and fleq/fltq differ from
	// FLE/FLT only in which NaNs raise invalid.
	bool ZFA = true;
	// Zfhmin, not Zfh: RVA23U64 mandates the minimal half-precision set
	// (load/store, bit moves, conversions) and makes full half-precision
	// arithmetic an expansion option. Software is expected to widen to
	// single, compute, and narrow back.
	bool ZFHMIN = true;

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
