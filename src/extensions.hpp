#pragma once

namespace Extensions {
	constexpr bool I = true;
	constexpr bool M = true;
	constexpr bool A = true;
	constexpr bool C = true;
	constexpr bool ZICSR = true;
	constexpr bool V = false;

	// Base ISA width, not an optional extension -- compile-time only
	// (flip and rebuild both the host and the guest, see tools/doombuild's
	// Makefile XLEN variable). Registers/Memory always store values in
	// 64-bit containers regardless of this flag: RV32 mode just means
	// every integer op computes at 32-bit width and sign-extends its
	// result into that container (the same mechanism RV64's *W-suffixed
	// instructions already use), rather than every register genuinely
	// being a narrower type. That keeps Registers/Memory/Snapshot/Gui
	// untouched by this flag -- only the decoder (RV64-only opcodes,
	// RV32-vs-RV64 compressed-encoding meanings) and RiscvCore's compute
	// width need to branch on it.
	constexpr bool XLEN64 = true;
}
